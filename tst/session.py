"""A real compositor for one scenario: an isolated headless Sway, the
driver's virtual input devices on it, and the tool under test as its
client. Scenarios drive the tool through real input and check what it
draws (grim) and what it leaves on disk."""

import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import tempfile
import time
import zlib

# the tool under test is spawned by its link's name, as the compositor
# spawns it: every scenario says which tool, this one has one
SCREENSHOT = "imscreenshot"

# the evdev codes the scenarios press
KEY_ESC = 1
KEY_ZERO = 11
KEY_MINUS = 12
KEY_EQUAL = 13
KEY_ENTER = 28
KEY_LEFTCTRL = 29
KEY_A = 30
BTN_LEFT = 272
BTN_RIGHT = 273
BTN_MIDDLE = 274


class Session:
    def __init__(self, name, width=1280, height=800):
        self.name = name
        self.binary = Path(os.environ["IM_E2E_BINARY"]).resolve()
        self.devices_binary = Path(os.environ["IM_E2E_DEVICES"]).resolve()
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
            self.compositor = self.start(["sway", "-d", "-c", str(config)], "sway")
            self.wait(lambda: next(runtime.glob("wayland-*[0-9]"), None), "Wayland socket", client=False)
            wayland = next(runtime.glob("wayland-*[0-9]"))
            self.socket = self.wait(lambda: next(runtime.glob("sway-ipc.*.sock"), None), "Sway IPC", client=False)
            self.env["WAYLAND_DISPLAY"] = wayland.name
            self.env["SWAYSOCK"] = str(self.socket)
            self.devices = self.start([str(self.devices_binary)], "devices", stdin=subprocess.PIPE)
            self.wait(lambda: "READY" in (self.artifacts / "devices.log").read_text(), "virtual devices", client=False)
            self.wait(lambda: any(item["type"] == "keyboard" for item in json.loads(
                self.command("swaymsg", "-r", "-t", "get_inputs"))), "virtual keyboard", client=False)
            # the tool by its link's name, as the compositor spawns it
            bin_dir = runtime / "bin"
            bin_dir.mkdir()
            os.symlink(self.binary, bin_dir / SCREENSHOT)
            self.env["PATH"] = f"{bin_dir}{os.pathsep}{self.env.get('PATH', '')}"
            return self
        except BaseException:
            self.cleanup()
            raise

    def environment(self, unset, overrides):
        env = {key: value for key, value in self.env.items() if key not in unset}
        env.update(overrides)
        return env

    def start(self, command, label, stdin=None, cwd=None, unset=(), **environment):
        log = (self.artifacts / f"{label}.log").open("wb")
        self.logs.append(log)
        process = subprocess.Popen(
            command, env=self.environment(unset, environment), stdout=log, stderr=subprocess.STDOUT,
            start_new_session=False, stdin=stdin, cwd=cwd,
        )
        self.processes.append(process)
        return process

    # ---- the tool under test ----

    def launch(self, *args, cwd=None, mapped=True, unset=(), **environment):
        """Start imscreenshot with args and the given environment (the
        compositor's IMWAY_SHOT_* words, IMGUI_SCALE, IM_CHAOS; unset names
        the variables it must not see); a mapped launch waits for its
        window."""
        self.clients += 1
        self.client = self.start([SCREENSHOT, *args], f"client{self.clients}", cwd=cwd, unset=unset, **environment)
        if mapped:
            self.wait(lambda: self.windows(), "mapped window")
        return self.client

    def run(self, *args, cwd=None, timeout=60, unset=(), **environment):
        """Run imscreenshot to its end without a window: the exit status and
        its output."""
        self.clients += 1
        log = self.artifacts / f"client{self.clients}.log"
        with log.open("wb") as out:
            process = subprocess.run(
                [SCREENSHOT, *args], env=self.environment(unset, environment), stdout=out, stderr=subprocess.STDOUT,
                cwd=cwd, timeout=timeout,
            )
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

    def button(self, code=BTN_LEFT, pressed=True):
        self.input(f"button {code} {int(pressed)}")

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
        return self.wait(lambda: self.client_log().count(f"im screenshot: {what}") >= times, f"the tool saying {what!r} {times}x")

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

    def stacks(self, label):
        """The tool's threads' stacks, kept as <label>.stack: a scenario that
        timed out on a tool still alive says where it stands."""
        gdb = shutil.which("gdb")
        if gdb is None:
            return
        try:
            result = subprocess.run(
                [gdb, "-p", str(self.client.pid), "-batch", "-nx", "-ex", "set pagination off", "-ex", "thread apply all bt",
                 "-ex", "detach", "-ex", "quit"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            )
            text = result.stdout.decode(errors="replace")
        except (OSError, subprocess.TimeoutExpired) as error:
            text = f"gdb failed: {error}\n"
        (self.artifacts / f"{label}.stack").write_text(text)

    def __exit__(self, kind, value, traceback):
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


def png_size(path):
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG"
    return struct.unpack(">II", data[16:24])


def is_jxl(path):
    data = Path(path).read_bytes()
    return data[:2] == b"\xff\x0a" or data[:12] == b"\x00\x00\x00\x0cJXL \r\n\x87\n"
