#!/usr/bin/env python3
"""Exercise first-run password setup and MOSdesk login through QEMU QMP."""

from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from contextlib import contextmanager


PASSWORD = "mosdesktest42"
MOSWM_STARTED = "'mosWM' started (PID "


@contextmanager
def test_workdir():
    workdir = Path(tempfile.mkdtemp(prefix="mosdesk-login-"))
    try:
        yield workdir
    except Exception:
        print(f"[mosdesk-login] Debug artifacts: {workdir}", file=sys.stderr)
        for serial_path in workdir.glob("*.serial.log"):
            lines = serial_path.read_text(errors="replace").splitlines()
            print(f"--- {serial_path.name} (last 30 lines) ---", file=sys.stderr)
            print("\n".join(lines[-30:]), file=sys.stderr)
        raise
    else:
        shutil.rmtree(workdir)


class QMPClient:
    def __init__(self, socket_path: Path, input_device: str,
                 timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(1.0)
        while True:
            try:
                self.socket.connect(str(socket_path))
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"QMP socket did not open: {socket_path}")
                time.sleep(0.1)

        self.reader = self.socket.makefile("rb")
        self.next_id = 1
        self.input_device = input_device
        greeting = json.loads(self.reader.readline())
        if "QMP" not in greeting:
            raise RuntimeError(f"Unexpected QMP greeting: {greeting}")
        self.execute("qmp_capabilities")

    def execute(self, command: str, arguments: dict | None = None):
        command_id = self.next_id
        self.next_id += 1
        request = {"execute": command, "id": command_id}
        if arguments is not None:
            request["arguments"] = arguments
        if command == "input-send-event":
            request.setdefault("arguments", {})["device"] = self.input_device
        self.socket.sendall((json.dumps(request) + "\n").encode("ascii"))

        while True:
            response = json.loads(self.reader.readline())
            if response.get("id") != command_id:
                continue
            if "error" in response:
                raise RuntimeError(f"QMP {command} failed: {response['error']}")
            return response.get("return")

    def move_mouse(self, dx: int, dy: int) -> None:
        self.execute(
            "input-send-event",
            {
                "events": [
                    {"type": "rel", "data": {"axis": "x", "value": dx}},
                    {"type": "rel", "data": {"axis": "y", "value": dy}},
                ]
            },
        )

    def click_left(self) -> None:
        for down in (True, False):
            self.execute(
                "input-send-event",
                {
                    "events": [
                        {"type": "btn", "data": {"button": "left", "down": down}},
                    ]
                },
            )
            time.sleep(0.1)

    def press_key(self, key: str) -> None:
        for down in (True, False):
            self.execute(
                "input-send-event",
                {
                    "events": [
                        {
                            "type": "key",
                            "data": {
                                "key": {"type": "qcode", "data": key},
                                "down": down,
                            },
                        },
                    ]
                },
            )
            time.sleep(0.03)

    def type_password(self, password: str) -> None:
        for character in password:
            self.press_key(character)

    def screendump(self, path: Path):
        self.execute("screendump", {"filename": str(path)})
        return read_ppm(path)

    def close(self) -> None:
        try:
            self.reader.close()
        finally:
            self.socket.close()


class QEMUInstance:
    def __init__(self, iso: Path, drive: Path, workdir: Path,
                 qemu_binary: str, timeout: int, label: str) -> None:
        self.serial_path = workdir / f"{label}.serial.log"
        self.qmp_path = workdir / f"{label}.qmp"
        self.stderr_file = (workdir / f"{label}.qemu.log").open("wb")
        command = [
            qemu_binary,
            "-smp", "2",
            "-cdrom", str(iso),
            "-m", "1024M",
            "-boot", "d",
            "-snapshot",
            "-vga", "none",
            "-device", "bochs-display,id=video0",
            "-no-reboot",
            "-no-shutdown",
            "-display", "none",
            "-monitor", "none",
            "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
            "-serial", f"file:{self.serial_path}",
            "-device", "piix3-usb-uhci,id=uhci",
            "-device", "usb-kbd,bus=uhci.0,port=1",
            "-device", "usb-mouse,bus=uhci.0,port=2",
            "-audiodev", "none,id=speaker",
            "-machine", "pcspk-audiodev=speaker",
            "-audiodev", "none,id=audio0",
            "-device", "AC97,audiodev=audio0",
            "-device", "ahci,id=ahci",
            "-drive", f"id=disk0,file={drive},if=none,format=raw",
            "-device", "ide-hd,drive=disk0,bus=ahci.0",
            "-netdev", "user,id=net0",
            "-device", "rtl8139,netdev=net0,mac=52:54:00:12:34:56",
        ]
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=self.stderr_file,
        )
        try:
            self.qmp = QMPClient(self.qmp_path, "video0", timeout=timeout)
        except Exception:
            self.stop()
            raise

    def wait_for_serial(self, marker: str, timeout: int) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"QEMU exited ({self.process.returncode}) before {marker!r}"
                )
            try:
                output = self.serial_path.read_text(errors="replace")
            except FileNotFoundError:
                output = ""
            if marker in output:
                return
            time.sleep(0.2)
        raise TimeoutError(f"Timed out waiting for {marker!r} in QEMU serial")

    def wait_for_serial_count(self, marker: str, count: int, timeout: int) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"QEMU exited ({self.process.returncode}) before boot {count}"
                )
            try:
                output = self.serial_path.read_text(errors="replace")
            except FileNotFoundError:
                output = ""
            if output.count(marker) >= count:
                return
            time.sleep(0.2)
        raise TimeoutError(f"Timed out waiting for boot {count} marker {marker!r}")

    def stop(self) -> None:
        qmp = getattr(self, "qmp", None)
        if qmp is not None:
            try:
                qmp.execute("quit")
            except (OSError, RuntimeError, ValueError):
                pass
            qmp.close()
            self.qmp = None
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.stderr_file.close()

    def __enter__(self):
        return self

    def __exit__(self, _exc_type, _exc_value, _traceback) -> None:
        self.stop()


def read_ppm(path: Path):
    data = path.read_bytes()
    offset = 0
    tokens = []
    while len(tokens) < 4:
        while offset < len(data) and data[offset] in b" \t\r\n":
            offset += 1
        if offset < len(data) and data[offset] == ord("#"):
            end = data.find(b"\n", offset)
            if end < 0:
                raise ValueError(f"Malformed PPM header: {path}")
            offset = end + 1
            continue
        end = offset
        while end < len(data) and data[end] not in b" \t\r\n#":
            end += 1
        tokens.append(data[offset:end])
        offset = end

    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError(f"Unsupported QEMU PPM format: {path}")
    if data[offset:offset + 2] == b"\r\n":
        offset += 2
    else:
        offset += 1
    width, height = int(tokens[1]), int(tokens[2])
    pixels = data[offset:]
    if len(pixels) < width * height * 3:
        raise ValueError(f"Truncated QEMU screendump: {path}")
    return width, height, pixels[:width * height * 3]


def changed_pixels(before, after, region=None) -> int:
    width, height, before_pixels = before
    after_width, after_height, after_pixels = after
    if (width, height) != (after_width, after_height):
        raise AssertionError("Display resolution changed during login test")

    if region is None:
        left, top, right, bottom = 0, 0, width, height
    else:
        left, top, right, bottom = region
    changed = 0
    for y in range(top, bottom):
        row_start = (y * width + left) * 3
        row_end = (y * width + right) * 3
        for offset in range(row_start, row_end, 3):
            difference = (
                abs(before_pixels[offset] - after_pixels[offset])
                + abs(before_pixels[offset + 1] - after_pixels[offset + 1])
                + abs(before_pixels[offset + 2] - after_pixels[offset + 2])
            )
            if difference > 24:
                changed += 1
    return changed


def dialog_bounds(width: int, height: int):
    dialog_width = 520 if width > 552 else width - 32
    dialog_height = 300 if height > 332 else height - 32
    x = (width - dialog_width) // 2
    y = (height - dialog_height) // 2
    return x, y, dialog_width, dialog_height


def wait_for_auth_screen(qmp: QMPClient, path: Path, timeout: float = 20.0):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        latest = qmp.screendump(path)
        width, height, pixels = latest
        x, y, dialog_width, _dialog_height = dialog_bounds(width, height)
        sample = ((y + 80) * width + x + dialog_width // 2) * 3
        if min(pixels[sample:sample + 3]) > 200:
            return latest
        time.sleep(0.2)
    raise AssertionError(f"MOSdesk auth dialog was not rendered in {path}")


def move_cursor_to(qmp: QMPClient, screen, x: int, y: int) -> None:
    width, height, _pixels = screen
    qmp.move_mouse(x - width // 2, y - height // 2)
    time.sleep(0.2)
    moved = qmp.screendump(Path(tempfile.gettempdir()) / "mosdesk-cursor-moved.ppm")
    region = (max(0, x - 16), max(0, y - 16),
              min(width, x + 17), min(height, y + 17))
    if changed_pixels(screen, moved, region) < 3:
        raise AssertionError(f"Mouse cursor did not move to ({x}, {y})")


def wait_for_screen_change(qmp: QMPClient, path: Path, before,
                           minimum_pixels: int, timeout: float = 5.0):
    deadline = time.monotonic() + timeout
    latest = before
    while time.monotonic() < deadline:
        latest = qmp.screendump(path)
        if changed_pixels(before, latest) >= minimum_pixels:
            return latest
        time.sleep(0.2)
    raise AssertionError(
        f"Screen did not transition (changed {changed_pixels(before, latest)} pixels)"
    )


def run_test(args) -> None:
    iso = args.iso.resolve()
    source_drive = args.disk.resolve()
    if not iso.is_file():
        raise FileNotFoundError(f"Kernel ISO not found: {iso}; run make MOSDESK=1 build-x86_64")
    if not source_drive.is_file():
        raise FileNotFoundError(f"Test disk not found: {source_drive}")
    shutil.which(args.qemu)

    with test_workdir() as workdir:
        print("[mosdesk-login] Booting first-run setup")
        with QEMUInstance(iso, source_drive, workdir, args.qemu,
                          args.timeout, "login") as vm:
            vm.wait_for_serial(MOSWM_STARTED, args.timeout)
            first_choice_path = workdir / "first-choice.ppm"
            choice_screen = wait_for_auth_screen(vm.qmp, first_choice_path)
            width, height, _pixels = choice_screen
            x, y, dialog_width, dialog_height = dialog_bounds(width, height)
            button_width = (dialog_width - 84) // 2
            set_password_x = x + 28 + button_width // 2
            set_password_y = y + dialog_height - 58 + 18

            move_cursor_to(vm.qmp, choice_screen, set_password_x, set_password_y)
            vm.qmp.click_left()
            setup_screen = wait_for_screen_change(
                vm.qmp, workdir / "password-setup.ppm", choice_screen,
                minimum_pixels=1000,
            )

            field_region = (x + 28, y + 140, x + dialog_width - 28, y + 178)
            move_cursor_to(vm.qmp, setup_screen,
                           x + dialog_width // 2, y + 159)
            vm.qmp.click_left()
            setup_before_password = vm.qmp.screendump(
                workdir / "setup-field-empty.ppm"
            )
            vm.qmp.type_password(args.password)
            time.sleep(0.3)
            setup_with_password = vm.qmp.screendump(
                workdir / "setup-field-filled.ppm"
            )
            if changed_pixels(setup_before_password, setup_with_password,
                              field_region) < 20:
                raise AssertionError("Keyboard input did not appear in setup password field")
            vm.qmp.press_key("ret")
            wait_for_screen_change(
                vm.qmp, workdir / "setup-desktop.ppm", setup_with_password,
                minimum_pixels=width * height // 20,
            )
            print("[mosdesk-login] Password setup reached the desktop")

            print("[mosdesk-login] Rebooting to test sign-in")
            vm.qmp.execute("system_reset")
            vm.wait_for_serial_count(MOSWM_STARTED, 2, args.timeout)
            login_screen = wait_for_auth_screen(
                vm.qmp, workdir / "login-screen.ppm"
            )
            width, height, _pixels = login_screen
            x, y, dialog_width, dialog_height = dialog_bounds(width, height)
            login_text_region = (x + 24, y + 78, x + dialog_width - 24, y + 136)
            setup_text_screen = read_ppm(workdir / "password-setup.ppm")
            if changed_pixels(setup_text_screen, login_screen,
                              login_text_region) < 50:
                raise AssertionError("Second boot did not show the sign-in prompt")

            move_cursor_to(vm.qmp, login_screen,
                           x + dialog_width // 2, y + 159)
            vm.qmp.click_left()
            empty_field = vm.qmp.screendump(workdir / "login-field-empty.ppm")
            vm.qmp.type_password(args.password)
            time.sleep(0.3)
            filled_field = vm.qmp.screendump(workdir / "login-field-filled.ppm")
            field_region = (x + 28, y + 140, x + dialog_width - 28, y + 178)
            if changed_pixels(empty_field, filled_field, field_region) < 20:
                raise AssertionError("Keyboard input did not appear in login password field")
            vm.qmp.press_key("ret")
            wait_for_screen_change(
                vm.qmp, workdir / "login-desktop.ppm", filled_field,
                minimum_pixels=width * height // 20,
            )
        print("[mosdesk-login] PASS: mouse click, password entry, and sign-in")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=Path("dist/x86_64/kernel.iso"))
    parser.add_argument("--disk", type=Path, default=Path("sata256.img"))
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--password", default=PASSWORD)
    args = parser.parse_args()
    if len(args.password) < 4 or len(args.password) > 63 or not args.password.isalnum() or not args.password.isascii() or not args.password.islower():
        parser.error("--password must be 4-63 lowercase ASCII letters or digits")

    try:
        run_test(args)
    except Exception as error:
        print(f"[mosdesk-login] FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())