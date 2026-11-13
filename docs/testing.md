# Testing

Minimal-OS has host-side unit tests, QEMU boot checks, interactive serial
tests, and focused integration scripts. Pick the narrowest test that exercises
the changed behavior, then add a boot test when the change crosses into kernel
startup or device integration.

## Host-side tests

Run the C tests that do not require a guest OS:

```sh
make test-host
```

The target builds and runs:

| Test binary | Main coverage |
| --- | --- |
| `gfx_blit_tests` | Framebuffer blit and graphics helper behavior |
| `fgpu_raster_tests` | FGPU rasterizer and shader execution |
| `fgsl_tests` | FGSL shader compilation and execution path |
| `fgpu_ir_tests` | FGPU intermediate representation |
| `nv_hw_tests` | NVIDIA firmware, VBIOS, and resource-manager data parsing |
| `hid_mouse_tests` | HID mouse report decoding |
| `media_tests` | Host-testable media interfaces |
| `os0_tests` | OS0 host helpers: time conversion, path resolution and the program argument block |

These are ordinary host binaries under `build/tests/host`. The Makefile uses
`HOSTCC` (default `gcc`) and `HOST_CFLAGS` for their compilation.

## QEMU boot smoke test

```sh
make ci-test
```

This builds the ISO, launches a headless QEMU guest with 512 MiB of memory,
and waits up to 60 seconds for known boot markers on the serial console. It
writes `ci_serial.log` in the repository root and exits nonzero if no marker
appears. It verifies that the image reaches a basic boot milestone; it does
not validate every device or command.

Required host tools include the normal kernel build dependencies and
`qemu-system-x86_64`.

## OS0 self-test in QEMU

```sh
make ci-os0
```

This builds the ISO, boots it headless in QEMU (2 CPUs, 1024 MiB, KVM only if
`/dev/kvm` is writable, otherwise TCG) and selects the "Minimal-OS (RamFS)"
GRUB entry through the QEMU monitor, so no disk image is needed: `0:` is a RAM
disk filled from the embedded package, including `0:/programs/os0test.run`.
Once `=== TERMINAL READY ===` appears (up to 120 seconds) the script types
`run 0:/programs/os0test.run` over the serial port, which the terminal reads
like keyboard input, and waits up to 600 seconds for a line ending in
`OS0TEST PASS` or `OS0TEST FAIL` in the serial log. The log is `ci_os0_serial.log` in the
repository root. The script exits 0 on PASS and nonzero on FAIL or timeout,
printing the tail of the log. Set `QEMU_BIN` to use another QEMU binary.

## Interactive serial test

```sh
make ci-interact
```

The script builds the ISO, starts headless QEMU with a TCP serial console, and
sends a small sequence of terminal commands. By default it uses port 5555,
drive number 0, and a 90-second timeout. The command-line options are:

```text
--drive N       drive number passed to storage commands
--delay S       pause between commands
--port P        serial TCP port
--timeout S     wait limit
--junit FILE    output JUnit XML at this path
--auto-mount    answer yes if the mount prompt appears
```

The default JUnit output is `tools/debug/ci_interact.xml`; serial output is
captured in `ci_serial_interact.log`. QEMU is stopped when the script exits.
Use a free local TCP port if the default is already in use.

## Focused integration checks

Useful focused checks include:

```sh
bash tools/debug/https_test.sh
make dlr-cli-test
```

The HTTPS script exercises the TLS client against a local OpenSSL server via
QEMU; it is an integration check, not a certificate-trust test. The Deliver
CLI test uses the helper in `tools/debug/test_dlr_present.sh`.

For a small cryptographic known-answer test, the source and build command are
documented in [Build and Debugging](build-and-debugging.md#https-test).

## Choose a test by change

| Change | Suggested first check |
| --- | --- |
| Pure host helper or parser | Add or update a host test, then `make test-host` |
| Kernel compile/link, resource packaging | `make build-x86_64` |
| Boot sequence, terminal, or kernel service startup | `make ci-test` |
| Terminal commands or storage interactions | `make ci-interact` |
| TLS/network client behavior | `bash tools/debug/https_test.sh` |
| SDK or Deliver CLI behavior | SDK build and `make dlr-cli-test` as applicable |
| Audio, USB, GPU, or real-device integration | Host tests first, then an appropriate QEMU or hardware test |

Tests that rely on QEMU hardware emulation do not establish that a driver works
on physical hardware. The hardware guide records known device support and
limitations.

## When a test fails

1. Re-run the smallest failing test and preserve its exit status.
2. Read the serial or QEMU output named by the test before changing code.
3. Distinguish a build failure from a boot-marker timeout or a guest command
   failure.
4. Remove or archive generated logs only after collecting the failure
   evidence; do not treat logs as source files.
5. Run `git status --short` before and after the test to identify generated
   files and keep them out of the change unless intentionally needed.
