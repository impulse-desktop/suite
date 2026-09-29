"""Real compositor fixture. Programs receive configuration only via environment."""

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


class Session:
    def __init__(self, name):
        self.name = name
        self.binary = Path(os.environ["PLT_E2E_BINARY"]).resolve()
        self.artifacts = Path(os.environ["PLT_E2E_ARTIFACTS"]).resolve()
        self.artifacts.mkdir(parents=True, exist_ok=True)
        self.processes = []
        self.logs = []
        self.client = None
        self.input_serial = 0
        self.socket = None
        self.runtime = None
        self.env = os.environ.copy()
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK"):
            self.env.pop(name, None)

    def __enter__(self):
        try:
            for tool in ("sway", "swaymsg", "grim", "wtype", "wl-copy", "wl-paste"):
                if shutil.which(tool) is None:
                    raise RuntimeError(f"required e2e tool missing: {tool}")
            self.runtime = tempfile.TemporaryDirectory(prefix="plt-e2e-")
            runtime = Path(self.runtime.name)
            runtime.chmod(0o700)
            self.env.update(
                XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(runtime / "cache"), WLR_BACKENDS="headless",
                WLR_HEADLESS_OUTPUTS="1", WLR_LIBINPUT_NO_DEVICES="1",
                WLR_RENDERER="pixman", DBUS_SESSION_BUS_ADDRESS="unix:path=/dev/null",
            )
            config = self.artifacts / "sway.config"
            config.write_text(
                'xwayland disable\n'
                'output HEADLESS-1 mode 1024x768\n'
                'output * scale 1\n'
                'output * bg #101010 solid_color\n'
                'seat seat0 fallback true\n'
                'seat seat0 hide_cursor 100\n'
                'default_border none\n'
                'default_floating_border none\n'
                'focus_follows_mouse no\n'
                'for_window [app_id="^plt-"] floating enable\n'
                'for_window [app_id="^plt-"] move position 40 50\n'
                'input * xkb_layout us\n'
            )
            self.compositor = self.start(["sway", "-d", "-c", str(config)], "sway")
            self.wait(lambda: next(runtime.glob("wayland-*[0-9]"), None), "Wayland socket", client=False)
            wayland = next(runtime.glob("wayland-*[0-9]"))
            self.socket = self.wait(lambda: next(runtime.glob("sway-ipc.*.sock"), None), "Sway IPC", client=False)
            self.env["WAYLAND_DISPLAY"] = wayland.name
            self.env["SWAYSOCK"] = str(self.socket)
            self.devices = self.start([os.environ["PLT_E2E_DEVICES"]], "devices", stdin=subprocess.PIPE)
            self.wait(lambda: "READY" in (self.artifacts / "devices.log").read_text(), "virtual pointer", client=False)
            self.start(["wtype", "-s", "60000"], "keyboard")
            self.wait(lambda: any(item["type"] == "keyboard" for item in json.loads(
                self.command("swaymsg", "-r", "-t", "get_inputs"))), "virtual keyboard", client=False)
            (self.artifacts / "environment.json").write_text(json.dumps({
                key: self.env.get(key) for key in (
                    "PLT_E2E_RENDERER", "WLR_RENDERER", "VK_DRIVER_FILES", "VK_ICD_FILENAMES",
                )
            }, indent=2))
            return self
        except BaseException:
            self.cleanup()
            raise

    def start(self, command, label, stdin=None, **environment):
        log = (self.artifacts / f"{label}.log").open("wb")
        self.logs.append(log)
        process = subprocess.Popen(
            command, env={**self.env, **environment}, stdout=log, stderr=subprocess.STDOUT,
            start_new_session=False, stdin=stdin,
        )
        self.processes.append(process)
        return process

    def launch(self, **environment):
        self.client = self.start([str(self.binary)], "client", **environment)
        self.wait(lambda: self.windows(), "mapped window")
        if self.env.get("PLT_E2E_RENDERER") == "lavapipe":
            self.logged("VULKAN ")
        return self.client

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
        result = subprocess.run(
            args, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=10,
        )
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
            if (node.get("app_id") or "").startswith("plt-"):
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

    def focus(self, app_id=None):
        node = self.window(app_id)
        self.ipc(f'[con_id={node["id"]}] focus')
        return node

    def type(self, text):
        self.command("wtype", "--", text)

    def key(self, key, modifier=None):
        args = ["wtype"]
        if modifier:
            args.extend(["-M", modifier])
        args.extend(["-k", key])
        if modifier:
            args.extend(["-m", modifier])
        self.command(*args)

    def input(self, command):
        self.input_serial += 1
        self.devices.stdin.write((command + "\n").encode())
        self.devices.stdin.flush()
        self.wait(lambda: f"DONE {self.input_serial}\n" in (self.artifacts / "devices.log").read_text(), "input delivery")

    def pointer(self, x, y, app_id=None):
        r = self.window(app_id)["rect"]
        output = json.loads(self.command("swaymsg", "-r", "-t", "get_outputs"))[0]["rect"]
        self.input(f'move {r["x"] + x} {r["y"] + y} {output["width"]} {output["height"]}')

    def button(self, button="button1", pressed=True):
        if button in ("button4", "button5"):
            if pressed:
                self.input(f'scroll {1 if button == "button5" else -1}')
        else:
            code = {"button1": 272, "button2": 274, "button3": 273}[button]
            self.input(f"button {code} {int(pressed)}")

    def click(self, x, y, button="button1", app_id=None):
        self.pointer(x, y, app_id)
        self.button(button)
        self.button(button, False)

    def logged(self, text):
        return self.wait(lambda: text in (self.artifacts / "client.log").read_text(errors="replace"), repr(text))

    def capture(self, label, app_id=None):
        r = self.window(app_id)["rect"]
        geometry = f'{r["x"]},{r["y"]} {r["width"]}x{r["height"]}'
        raw = self.command("grim", "-t", "ppm", "-g", geometry, "-")
        match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\n", raw)
        if match is None:
            raise AssertionError("grim did not return an RGB PPM screenshot")
        width, height = map(int, match.groups())
        pixels = raw[match.end():]
        assert len(pixels) == width * height * 3
        # Store the exact captured pixels as PNG without a Pillow dependency.
        def chunk(kind, payload):
            return struct.pack("!I", len(payload)) + kind + payload + struct.pack("!I", zlib.crc32(kind + payload))
        rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
        (self.artifacts / f"{label}.png").write_bytes(png)
        (self.artifacts / f"{label}.tree.json").write_text(json.dumps(self.windows(), indent=2))
        return width, height, pixels

    def screenshot(self, label, regions, app_id=None):
        """Wait for presentation, then assert every pixel in normalized rectangles."""
        last = ""
        def matches():
            nonlocal last
            width, height, pixels = self.capture(label, app_id)
            for x0, y0, x1, y1, color in regions:
                expected = bytes.fromhex(color)
                for y in range(int(y0 * height), int(y1 * height)):
                    row = pixels[(y * width + int(x0 * width)) * 3:(y * width + int(x1 * width)) * 3]
                    if row != expected * (int(x1 * width) - int(x0 * width)):
                        last = f"region {(x0, y0, x1, y1)} expected #{color}"
                        return False
            return True
        try:
            self.wait(matches, f"screenshot {label}")
        except AssertionError as error:
            raise AssertionError(f"{error}: {last}") from error

    def close(self):
        self.key("Escape")
        assert self.client.wait(timeout=10) == 0
        self.client = None

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

    def __exit__(self, kind, value, traceback):
        try:
            if kind is not None and self.client and self.client.poll() is None:
                try:
                    self.capture("failure")
                except Exception:
                    pass
            log_path = self.artifacts / "client.log"
            if kind is None and log_path.exists():
                log = log_path.read_text(errors="replace")
                assert "Validation Error" not in log and "VUID-" not in log, "Vulkan validation error"
        finally:
            self.cleanup()
