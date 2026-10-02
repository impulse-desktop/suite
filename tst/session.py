"""A real compositor for one scenario: an isolated headless Sway, the
driver's virtual input devices on it, and the tool under test as its
client. Scenarios drive the tool through real input and check what it
draws (grim) and what it leaves on disk."""

import fcntl
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib

# the exit status of a skipped scenario, as dev/run_test.py reads it
SKIPPED = 77

# the tools under test are spawned by their links' names, as the
# compositor spawns them; a session names its tool
SCREENSHOT = "imscreenshot"
VIEW = "imview"
UI = "imui"
TOOLS = {"screenshot": SCREENSHOT, "view": VIEW, "play": "implay", "ui": UI}

# the evdev codes the scenarios press
KEY_ESC = 1
KEY_1 = 2
KEY_ZERO = 11
KEY_MINUS = 12
KEY_EQUAL = 13
KEY_BACKSPACE = 14
KEY_TAB = 15
KEY_Q = 16
KEY_W = 17
KEY_R = 19
KEY_ENTER = 28
KEY_LEFTCTRL = 29
KEY_A = 30
KEY_F = 33
KEY_G = 34
KEY_J = 36
KEY_K = 37
KEY_LEFTSHIFT = 42
KEY_B = 48
KEY_SPACE = 57
KEY_HOME = 102
KEY_UP = 103
KEY_PAGEUP = 104
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_END = 107
KEY_DOWN = 108
KEY_PAGEDOWN = 109
BTN_LEFT = 272
BTN_RIGHT = 273
BTN_MIDDLE = 274


class Skip(Exception):
    """The compositor lacks what the scenario needs; the scenario ends as
    skipped, saying what."""


class Session:
    def __init__(self, name, width=1280, height=800, tool="screenshot"):
        self.name = name
        self.tool = tool
        self.link = TOOLS[tool]
        self.binary = Path(os.environ["IM_E2E_BINARY"]).resolve()
        helpers = Path(os.environ["IM_E2E_HELPERS"]).resolve()
        self.devices_binary = helpers / "devices"
        self.jxl_dump = helpers / "jxl_dump"
        self.device_uuid_binary = helpers / "device_uuid"
        self.globals = {}
        self.artifacts = Path(os.environ["IM_E2E_ARTIFACTS"]).resolve()
        self.artifacts.mkdir(parents=True, exist_ok=True)
        self.width = width
        self.height = height
        self.processes = []
        self.logs = []
        self.client = None
        self.clients = 0
        self.input_serial = 0
        self.socket = None
        self.runtime = None
        self.env = os.environ.copy()
        self.env["IM_SCALE"] = "1"
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK"):
            self.env.pop(key, None)

    def __enter__(self):
        try:
            for tool in ("sway", "swaymsg", "grim"):
                if shutil.which(tool) is None:
                    raise RuntimeError(f"required e2e tool missing: {tool}")
            # the socket path has 108 bytes: the runtime dir stays short
            self.runtime = tempfile.TemporaryDirectory(prefix="im-e2e-", dir=os.environ.get("IM_E2E_TMP"))
            runtime = Path(self.runtime.name)
            runtime.chmod(0o700)
            self.env.update(
                XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(runtime / "cache"), WLR_BACKENDS="headless",
                WLR_HEADLESS_OUTPUTS="1", WLR_LIBINPUT_NO_DEVICES="1",
                # pixman draws the compositor on the CPU (CI, with lavapipe
                # behind the tool); a GPU renderer offers the dma-bufs a
                # hardware Vulkan driver's swapchain wants (a developer's box)
                WLR_RENDERER=os.environ.get("IM_E2E_RENDERER", "pixman"),
                # the cursor drawn into the frame, never into a buffer of
                # its own: lavapipe's rasteriser faults on the ten by
                # sixteen the Vulkan renderer would give it
                WLR_NO_HARDWARE_CURSORS="1",
                DBUS_SESSION_BUS_ADDRESS="unix:path=/dev/null",
            )
            config = self.artifacts / "sway.config"
            config.write_text(
                'xwayland disable\n'
                f'output HEADLESS-1 mode {self.width}x{self.height}\n'
                'output * scale 1\n'
                'output * bg #101010 solid_color\n'
                'seat seat0 fallback true\n'
                'seat seat0 hide_cursor 100\n'
                'default_border none\n'
                'default_floating_border none\n'
                'focus_follows_mouse no\n'
                'for_window [app_id="^im-"] floating enable\n'
                'for_window [app_id="^im-"] move position 40 50\n'
                'input * xkb_layout us\n'
            )
            # the Vulkan validation layer is the tool's, not the compositor's;
            # IM_E2E_COMPOSITOR_GDB runs the compositor under gdb, for the
            # stack of a crash to land in its log
            sway = ["sway", "-d", "-c", str(config)]
            if os.environ.get("IM_E2E_COMPOSITOR_GDB") and shutil.which("gdb"):
                sway = ["gdb", "-batch", "-nx", "-ex", "set pagination off", "-ex", "run", "-ex", "thread apply all bt", "--args", *sway]
            self.compositor = self.start(sway, "sway", unset=("VK_INSTANCE_LAYERS",))
            self.wait(lambda: next(runtime.glob("wayland-*[0-9]"), None), "Wayland socket", client=False)
            wayland = next(runtime.glob("wayland-*[0-9]"))
            self.socket = self.wait(lambda: next(runtime.glob("sway-ipc.*.sock"), None), "Sway IPC", client=False)
            self.env["WAYLAND_DISPLAY"] = wayland.name
            self.env["SWAYSOCK"] = str(self.socket)
            self.devices = self.start([str(self.devices_binary)], "devices", stdin=subprocess.PIPE)
            self.wait(lambda: "READY" in (self.artifacts / "devices.log").read_text(), "virtual devices", client=False)
            for line in (self.artifacts / "devices.log").read_text().splitlines():
                word = line.split()
                if len(word) == 3 and word[0] == "GLOBAL":
                    self.globals[word[1]] = int(word[2])
            self.wait(lambda: any(item["type"] == "keyboard" for item in json.loads(
                self.command("swaymsg", "-r", "-t", "get_inputs"))), "virtual keyboard", client=False)
            # the tools by their links' names, as the compositor spawns them
            bin_dir = runtime / "bin"
            bin_dir.mkdir()
            for link in TOOLS.values():
                os.symlink(self.binary, bin_dir / link)
            self.env["PATH"] = f"{bin_dir}{os.pathsep}{self.env.get('PATH', '')}"
            return self
        except BaseException:
            self.cleanup()
            raise

    def require(self, interface):
        """The compositor offers this global, or the scenario is skipped."""
        if interface not in self.globals:
            raise Skip(f"the compositor offers no {interface}")

    def skip(self, reason):
        raise Skip(reason)

    def device_uuid(self):
        """The Vulkan device the tool gets, as a compositor would name it
        on a shared buffer."""
        return subprocess.run([str(self.device_uuid_binary)], env=self.env, stdout=subprocess.PIPE, check=True, timeout=30).stdout.decode().strip()

    def environment(self, unset, overrides):
        env = {key: value for key, value in self.env.items() if key not in unset}
        env.update(overrides)
        return env

    def start(self, command, label, stdin=None, cwd=None, unset=(), fd3=None, **environment):
        """A process of the session; fd3 names a descriptor it gets as its
        fd 3, as the compositor hands the tool a capture."""
        log = (self.artifacts / f"{label}.log").open("wb")
        self.logs.append(log)
        process = subprocess.Popen(
            command, env=self.environment(unset, environment), stdout=log, stderr=subprocess.STDOUT,
            start_new_session=False, stdin=stdin, cwd=cwd, **as_fd3(fd3),
        )
        self.processes.append(process)
        return process

    # ---- the tool under test ----

    def launch(self, *args, cwd=None, mapped=True, unset=(), fd3=None, **environment):
        """Start the session's tool with args and the given environment
        (the compositor's IM_SHOT_* words, IM_SCALE, IM_CHAOS; unset
        names the variables it must not see); a mapped launch waits for
        its window."""
        self.clients += 1
        self.client = self.start([self.link, *args], f"client{self.clients}", cwd=cwd, unset=unset, fd3=fd3, **environment)
        if mapped:
            self.wait(lambda: self.windows(), "mapped window")
        return self.client

    def run(self, *args, command=None, cwd=None, timeout=60, unset=(), fd3=None, **environment):
        """Run the session's tool (or the one binary itself, by its path
        as command) to its end without a window: the exit status and its
        output."""
        self.clients += 1
        log = self.artifacts / f"client{self.clients}.log"
        with log.open("wb") as out:
            process = subprocess.Popen(
                [str(command or self.link), *args], env=self.environment(unset, environment), stdout=out, stderr=subprocess.STDOUT,
                cwd=cwd, **as_fd3(fd3),
            )
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # where a tool that never ended stands, before it is killed
                self.stacks(f"client{self.clients}", process.pid)
                process.kill()
                process.wait()
                raise
        return process.returncode, log.read_text(errors="replace")

    def finished(self, timeout=10):
        """The tool exited; its status."""
        code = self.client.wait(timeout=timeout)
        self.client = None
        return code

    def client_log(self):
        return (self.artifacts / f"client{self.clients}.log").read_text(errors="replace")

    def logged(self, text):
        return self.wait(lambda: text in self.client_log(), repr(text))

    # ---- the compositor's view ----

    def wait(self, predicate, description, timeout=12, client=True):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.compositor.poll() is not None:
                raise RuntimeError(f"compositor exited: {self.compositor.returncode}")
            if client and self.client is not None and self.client.poll() is not None:
                raise RuntimeError(f"client exited: {self.client.returncode}; waiting for {description}")
            result = predicate()
            if result:
                return result
            time.sleep(0.04)
        raise AssertionError(f"timed out waiting for {description}")

    def command(self, *args):
        result = subprocess.run(args, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        if result.returncode:
            raise RuntimeError(f"{args}: {result.stderr.decode(errors='replace')}")
        return result.stdout

    def ipc(self, command):
        replies = json.loads(self.command("swaymsg", "-s", str(self.socket), "-r", command))
        if not all(reply.get("success") for reply in replies):
            raise AssertionError(f"sway command failed: {command}: {replies}")

    def windows(self):
        tree = json.loads(self.command("swaymsg", "-s", str(self.socket), "-r", "-t", "get_tree"))
        found = []

        def visit(node):
            if (node.get("app_id") or "").startswith("im-"):
                found.append(node)
            for child in node.get("nodes", []) + node.get("floating_nodes", []):
                visit(child)

        visit(tree)
        return found

    def window(self, app_id=None):
        return self.wait(
            lambda: next((node for node in self.windows() if app_id is None or node["app_id"] == app_id), None),
            f"window {app_id}",
        )

    def gone(self, timeout=12):
        self.wait(lambda: not self.windows(), "window gone", timeout=timeout, client=False)

    def focus(self, app_id=None):
        node = self.window(app_id)
        self.ipc(f'[con_id={node["id"]}] focus')
        return node

    def size(self, app_id=None):
        r = self.window(app_id)["rect"]
        return r["width"], r["height"]

    # ---- input ----

    def input(self, command, client=True):
        """One command to the devices, delivered to the compositor; a key
        that ends the tool is sent with client=False, as its release lands
        after the exit."""
        self.input_serial += 1
        self.devices.stdin.write((command + "\n").encode())
        self.devices.stdin.flush()
        self.wait(lambda: f"DONE {self.input_serial}\n" in (self.artifacts / "devices.log").read_text(), "input delivery", client=client)

    def key(self, code, state, client=True):
        self.input(f"key {code} {int(state)}", client=client)

    def tap(self, code, hold=0.0, client=True):
        """Press and release an evdev key; a hold long enough lets the
        client's own repeat start."""
        self.key(code, 1, client=client)
        if hold:
            time.sleep(hold)
        self.key(code, 0, client=client)
        time.sleep(0.05)

    def pointer(self, x, y, app_id=None):
        """The pointer to x,y of the window."""
        r = self.window(app_id)["rect"]
        self.input(f'move {r["x"] + x} {r["y"] + y} {self.width} {self.height}')

    def pointer_output(self, x, y):
        self.input(f"move {x} {y} {self.width} {self.height}")

    def button(self, code=BTN_LEFT, pressed=True, client=True):
        """A button press or release; one that ends the tool is sent with
        client=False, as for a key."""
        self.input(f"button {code} {int(pressed)}", client=client)

    def click(self, x, y, code=BTN_LEFT, app_id=None):
        self.pointer(x, y, app_id)
        self.button(code)
        self.button(code, False)

    def scroll(self, steps):
        self.input(f"scroll {steps}")

    def drag(self, x0, y0, x1, y1, code=BTN_LEFT, steps=4, pause=0.1, app_id=None):
        """A button held from x0,y0 to x1,y1 of the window, in steps, a
        moment each so the tool sees them as separate motions."""
        self.pointer(x0, y0, app_id)
        time.sleep(pause)
        self.pointer(x0 + 1, y0, app_id)
        time.sleep(pause)
        self.pointer(x0, y0, app_id)
        time.sleep(pause)
        self.button(code)
        time.sleep(pause)
        for i in range(1, steps + 1):
            self.pointer(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps, app_id)
            time.sleep(pause)
        self.button(code, False)
        time.sleep(0.3)

    def said(self, what, times=1):
        """The tool's own account of what it did (the test build's trace
        lines), the times-th time."""
        return self.wait(lambda: self.client_log().count(f"im {self.tool}: {what}") >= times, f"the tool saying {what!r} {times}x")

    # ---- pixels ----

    def capture(self, label, app_id=None, region=None):
        """The window (or a region x,y,w,h of it) as grim shows it: width,
        height, RGB bytes, kept as <label>.png with the window tree."""
        r = self.window(app_id)["rect"]
        if region:
            x, y, w, h = region
            geometry = f'{r["x"] + x},{r["y"] + y} {w}x{h}'
        else:
            geometry = f'{r["x"]},{r["y"]} {r["width"]}x{r["height"]}'
        raw = self.command("grim", "-t", "ppm", "-g", geometry, "-")
        match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\n", raw)
        if match is None:
            raise AssertionError("grim did not return an RGB PPM screenshot")
        width, height = map(int, match.groups())
        pixels = raw[match.end():]
        assert len(pixels) == width * height * 3
        (self.artifacts / f"{label}.png").write_bytes(png(width, height, pixels))
        (self.artifacts / f"{label}.tree.json").write_text(json.dumps(self.windows(), indent=2))
        return width, height, pixels

    def settled(self, label, app_id=None, region=None, pause=0.2, tolerance=60):
        """Two captures a moment apart that agree: a frame the tool has
        finished painting, not whatever a fixed sleep lands on."""
        def agree():
            first = self.capture(label, app_id, region)
            time.sleep(pause)
            second = self.capture(label, app_id, region)
            return first if differing(first, second) < tolerance else None
        return self.wait(agree, f"a settled frame {label}")

    def changed(self, baseline, label, app_id=None, region=None, threshold=500):
        """The window differs from the baseline capture by more than
        threshold pixels (the tool is a frame or more behind a key)."""
        return self.compared(baseline, label, app_id, region, lambda count: count > threshold, f"a change to {label}")

    def same(self, baseline, label, app_id=None, region=None, tolerance=60):
        return self.compared(baseline, label, app_id, region, lambda count: count < tolerance, f"{label} back to the baseline")

    def compared(self, baseline, label, app_id, region, accept, description):
        """Captures until one compares with the baseline as accept says; a
        timeout names how far the last one was off, and where."""
        last = None

        def capture():
            nonlocal last
            last = self.capture(label, app_id, region)
            return accept(differing(baseline, last))

        try:
            return self.wait(capture, description)
        except AssertionError:
            if last is None:
                raise
            raise AssertionError(f"timed out waiting for {description}: {differing(baseline, last)} pixels differ, within {differing_box(baseline, last)}") from None

    # ---- captures for the tool ----

    def capture_file(self, name, width, height, pixel=(255, 0, 255, 255), pixels=None):
        """An IMW1 capture, what the compositor hands the editor: a solid
        colour or the given RGBA8 bytes."""
        path = self.artifacts / name
        data = pixels if pixels is not None else bytes(pixel) * (width * height)
        path.write_bytes(struct.pack("<III", 0x31574D49, width, height) + data)
        return path

    def close(self, code=KEY_ESC):
        """Escape (or another key) leaves the tool; it must exit 0."""
        self.tap(code, client=False)
        self.gone()
        assert self.finished() == 0, "the tool did not exit cleanly"

    def cleanup(self):
        errors = []
        for process in reversed(self.processes):
            if process.stdin is not None:
                process.stdin.close()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    errors.append(f"process {process.pid} required SIGKILL")
        for log in self.logs:
            log.close()
        if self.runtime:
            self.runtime.cleanup()
        if errors:
            raise RuntimeError("; ".join(errors))

    def stacks(self, label, pid=None):
        """A tool's threads' stacks, kept as <label>.stack: a scenario that
        timed out on a tool still alive says where it stands."""
        gdb = shutil.which("gdb")
        if gdb is None:
            return
        try:
            result = subprocess.run(
                [gdb, "-p", str(pid or self.client.pid), "-batch", "-nx", "-ex", "set pagination off", "-ex", "thread apply all bt",
                 "-ex", "detach", "-ex", "quit"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            )
            text = result.stdout.decode(errors="replace")
        except (OSError, subprocess.TimeoutExpired) as error:
            text = f"gdb failed: {error}\n"
        (self.artifacts / f"{label}.stack").write_text(text)

    def __exit__(self, kind, value, traceback):
        if kind is Skip:
            self.cleanup()
            print(f"skip: {value}", flush=True)
            sys.exit(SKIPPED)
        try:
            if kind is not None and self.client and self.client.poll() is None:
                self.stacks(f"client{self.clients}")
                if self.windows():
                    try:
                        self.capture("failure")
                    except Exception:
                        pass
            if kind is None:
                for log in self.artifacts.glob("client*.log"):
                    text = log.read_text(errors="replace")
                    assert "Validation Error" not in text and "VUID-" not in text, f"Vulkan validation error in {log.name}"
        finally:
            self.cleanup()


def as_fd3(fd):
    """Popen arguments that hand a descriptor to the child as its fd 3.
    The hook's dup2 makes fd 3 inheritable in the child; nothing else is
    (Python's own descriptors close on exec), so close_fds stays off: on,
    Python would close that fresh fd 3 right after the hook, as it keeps
    only what pass_fds lists."""
    if fd is None:
        return {}

    def take():
        os.dup2(fd, 3)
        os.set_inheritable(3, True)

    return {"close_fds": False, "preexec_fn": take}


UDMABUF_CREATE = 0x40187542
UDMABUF_FLAGS_CLOEXEC = 1


def udmabuf(pixels):
    """A dma-buf holding these bytes, made through udmabuf from a sealed
    memfd, as the compositor hands the scanout buffer over; None where the
    kernel offers no udmabuf."""
    if not os.path.exists("/dev/udmabuf"):
        return None
    size = (len(pixels) + 4095) // 4096 * 4096
    memfd = os.memfd_create("shot", os.MFD_ALLOW_SEALING)
    os.ftruncate(memfd, size)
    os.pwrite(memfd, pixels, 0)
    fcntl.fcntl(memfd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SHRINK)
    with open("/dev/udmabuf", "r+b", buffering=0) as device:
        # a mutable buffer, so the call's own result, the new descriptor, comes back
        return fcntl.ioctl(device.fileno(), UDMABUF_CREATE, bytearray(struct.pack("=IIQQ", memfd, UDMABUF_FLAGS_CLOEXEC, 0, size)))


def write_png(path, width, height, rgb):
    """A solid PNG of the colour, as a file for the viewer."""
    Path(path).write_bytes(png(width, height, bytes(rgb) * (width * height)))


def near(capture, rgb, tolerance=3):
    """How many pixels of a capture are the colour, within the tolerance
    per channel."""
    w, h, px = capture
    r, g, b = rgb
    return sum(1 for i in range(0, w * h * 3, 3) if abs(px[i] - r) <= tolerance and abs(px[i + 1] - g) <= tolerance and abs(px[i + 2] - b) <= tolerance)


def colour_box(capture, accept):
    """The bounding box x0,y0,x1,y1 (inclusive) of the capture's pixels
    accept(r, g, b) takes, or None."""
    w, h, px = capture
    xs, ys = [], []
    for i in range(w * h):
        if accept(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]):
            xs.append(i % w)
            ys.append(i // w)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def differing(a, b):
    """Pixels that differ between two captures of one size."""
    (w, h, x), (_, _, y) = a, b
    return sum(1 for i in range(0, w * h * 3, 3) if x[i:i + 3] != y[i:i + 3])


def differing_box(a, b):
    """The bounding box, as x0,y0-x1,y1 (inclusive), of the pixels that
    differ between two captures of one size; 'nowhere' when none do."""
    (w, h, x), (_, _, y) = a, b
    rows = [i // w for i in range(w * h) if x[i * 3:i * 3 + 3] != y[i * 3:i * 3 + 3]]
    columns = [i % w for i in range(w * h) if x[i * 3:i * 3 + 3] != y[i * 3:i * 3 + 3]]
    if not rows:
        return "nowhere"
    return f"{min(columns)},{min(rows)}-{max(columns)},{max(rows)}"


def png(width, height, pixels):
    """The exact captured pixels as a PNG, with no Pillow dependency."""
    def chunk(kind, payload):
        return struct.pack("!I", len(payload)) + kind + payload + struct.pack("!I", zlib.crc32(kind + payload))
    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0))
    return data + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def jxl_pixels(session, path):
    """A saved JPEG XL as width, height and its 16-bit RGB samples in the
    image's own encoding (PQ code values for an HDR frame)."""
    out = session.artifacts / (Path(path).stem + ".rgb16")
    subprocess.run([str(session.jxl_dump), str(path), str(out)], check=True, timeout=30)
    data = out.read_bytes()
    header, _, pixels = data.partition(b"\n")
    width, height = map(int, header.split())
    return width, height, struct.unpack(f"<{width * height * 3}H", pixels)


def png_pixels(path):
    """A saved PNG's width, height and RGBA bytes: 8-bit, non-interlaced,
    as the tool writes it."""
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG"
    width, height, depth, kind = struct.unpack(">IIBB", data[16:26])
    assert (depth, kind) == (8, 6), f"{path}: not 8-bit RGBA"
    idat = b""
    at = 8
    while at < len(data):
        size, name = struct.unpack(">I4s", data[at:at + 8])
        if name == b"IDAT":
            idat += data[at + 8:at + 8 + size]
        at += 12 + size
    raw = zlib.decompress(idat)
    stride = width * 4
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = previous[i]
            c = previous[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        previous = line
    return width, height, b"".join(rows)


# ---- the tool's HDR arithmetic, for what it saves to be checked against
PQ_M1, PQ_M2, PQ_C1, PQ_C2, PQ_C3 = 2610 / 16384, 2523 / 32, 3424 / 4096, 2413 / 128, 2392 / 128
SDR_WHITE_NITS = 203.0


def pq_encode(nits):
    """The PQ code value, 0..1, of a luminance in nits."""
    p = max(nits / 10000, 0) ** PQ_M1
    return ((PQ_C1 + PQ_C2 * p) / (1 + PQ_C3 * p)) ** PQ_M2


def pq_decode(code):
    p = max(code, 0) ** (1 / PQ_M2)
    return (max(p - PQ_C1, 0) / (PQ_C2 - PQ_C3 * p)) ** (1 / PQ_M1) * 10000


def sdr_tone_map(nits):
    """The tool's display mapping of a neutral luminance onto the SDR range
    (screenshot.cpp toneMap for the SDR output: a knee at 90% of 203 nits)."""
    peak = SDR_WHITE_NITS
    knee = peak * 0.9
    if nits <= knee:
        return nits
    headroom = peak - knee
    return peak - headroom * headroom / (headroom + nits - knee)


def srgb8(linear):
    """An sRGB byte of a linear value in 0..1, as the tool encodes a PNG."""
    linear = min(max(linear, 0.0), 1.0)
    encoded = linear * 12.92 if linear <= 0.0031308 else 1.055 * linear ** (1 / 2.4) - 0.055
    return round(encoded * 255)


def hdr_png_grey(nits):
    """The byte a neutral pixel of this luminance lands on in the PNG the
    tool saves from a PQ frame: the PQ code decoded, mapped, sRGB at 203."""
    return srgb8(sdr_tone_map(nits) / SDR_WHITE_NITS)


# CIE xy chromaticities in millionths: red, green, blue, white
SRGB_PRIMARIES = (640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000)
BT2020_PRIMARIES = (708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000)


def _multiply(a, b):
    return [sum(a[row * 3 + k] * b[k * 3 + col] for k in range(3)) for row in range(3) for col in range(3)]


def _apply(m, c):
    return [m[0] * c[0] + m[1] * c[1] + m[2] * c[2], m[3] * c[0] + m[4] * c[1] + m[5] * c[2], m[6] * c[0] + m[7] * c[1] + m[8] * c[2]]


def _inverse(m):
    d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6])
    return [x / d for x in (
        m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
        m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
        m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3])]


def _rgb_to_xyz(c):
    xr, yr, xg, yg, xb, yb, xw, yw = (v / 1000000 for v in c)
    primaries = [xr / yr, xg / yg, xb / yb, 1, 1, 1, (1 - xr - yr) / yr, (1 - xg - yg) / yg, (1 - xb - yb) / yb]
    scale = _apply(_inverse(primaries), [xw / yw, 1, (1 - xw - yw) / yw])
    return [primaries[row * 3 + col] * scale[col] for row in range(3) for col in range(3)]


def hdr_png_pixel(nits):
    """The RGB bytes a linear BT.2020 colour, in nits, lands on in the PNG
    the tool saves from a PQ frame: screenshot.cpp's SDR mapping, the chroma
    held inside the sRGB gamut, then sRGB at 203."""
    scene = _rgb_to_xyz(BT2020_PRIMARIES)
    target = _rgb_to_xyz(SRGB_PRIMARIES)
    to_target = _multiply(_inverse(target), scene)
    from_target = _multiply(_inverse(scene), target)
    luma = target[3:6]
    t = _apply(to_target, nits)
    luminance = sum(l * c for l, c in zip(luma, t))
    mapped = sdr_tone_map(luminance)
    if luminance > 1e-9:
        t = [c * mapped / luminance for c in t]
    else:
        t, mapped = [0, 0, 0], 0
    chroma = 1.0
    for c in t:
        if c < 0:
            chroma = min(chroma, mapped / (mapped - c))
        elif c > SDR_WHITE_NITS:
            chroma = min(chroma, (SDR_WHITE_NITS - mapped) / (c - mapped))
    chroma = max(0.0, min(1.0, chroma))
    t = [mapped + (c - mapped) * chroma for c in t]
    out = _apply(to_target, _apply(from_target, t))
    return tuple(srgb8(c / SDR_WHITE_NITS) for c in out)


def png_size(path):
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG"
    return struct.unpack(">II", data[16:24])


def is_jxl(path):
    data = Path(path).read_bytes()
    return data[:2] == b"\xff\x0a" or data[:12] == b"\x00\x00\x00\x0cJXL \r\n\x87\n"


def write_video(path, seconds=12, audio=True, sizes=None, codec=None):
    """Indexed raw AVI with changing RGB frames and optional PCM audio.

    No encoder dependency: this exercises demux, decode, clocks and seeking
    with known frame times, including builds without external FFmpeg tools.
    sizes makes the frames PNGs of those sizes, one size per second; codec
    names another FourCC for the raw frames.
    """
    def chunk(kind, data):
        return kind + struct.pack("<I", len(data)) + data + b"\0" * (len(data) % 2)

    def group(kind, data):
        return chunk(b"LIST", kind + data)

    width, height, fps, rate = 64, 48, 25, 48000
    count = seconds * fps
    frame_size = width * height * 3
    main = struct.pack("<14I", 1000000 // fps, frame_size * fps, 0, 0x10, count, 0,
                       2 if audio else 1, frame_size, width, height, 0, 0, 0, 0)
    fourcc = codec or (b"MPNG" if sizes else b"DIB ")
    compression = 0 if fourcc == b"DIB " else int.from_bytes(fourcc, "little")
    video_header = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"vids", fourcc, 0, 0, 0, 0,
                               1, fps, 0, count, frame_size, 0xffffffff, 0, 0, 0, width, height)
    video_format = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, compression, frame_size, 0, 0, 0, 0)
    headers = chunk(b"avih", main) + group(b"strl", chunk(b"strh", video_header) + chunk(b"strf", video_format))
    pcm = b""
    if audio:
        audio_header = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"auds", b"\0" * 4, 0, 0, 0, 0,
                                   2, rate * 2, 0, seconds * rate, rate // fps * 2, 0xffffffff, 2, 0, 0, 0, 0)
        audio_format = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
        headers += group(b"strl", chunk(b"strh", audio_header) + chunk(b"strf", audio_format))
        pcm = b"".join(struct.pack("<h", round(2000 * math.sin(i * 2 * math.pi * 400 / rate))) for i in range(rate // fps))
    data, index = bytearray(), bytearray()
    for i in range(count):
        # The changing red channel catches stale texture contents.
        if sizes:
            w, h = sizes[min(i // fps, len(sizes) - 1)]
            frame = png(w, h, bytes((i % 256, 96, 32)) * (w * h))
            samples = [(b"00dc", frame)]
        else:
            samples = [(b"00dc" if codec else b"00db", bytes((32, 96, i % 256)) * (width * height))]
        if audio:
            samples.append((b"01wb", pcm))
        for kind, payload in samples:
            index.extend(struct.pack("<4sIII", kind, 0x10, len(data) + 4, len(payload)))
            data.extend(chunk(kind, payload))
    path.write_bytes(chunk(b"RIFF", b"AVI " + group(b"hdrl", headers) + group(b"movi", data) + chunk(b"idx1", index)))
