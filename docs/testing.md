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
| `fgpu_bin_tests` | Binned rasterizer against the immediate one (see below) |
| `fgsl_tests` | FGSL shader compilation and execution path |
| `fgpu_ir_tests` | FGPU intermediate representation |
| `fgpu_jit_tests` | Shader JIT (SSE4.1 and AVX2 machine code) against the interpreter, bit for bit: per-opcode edge cases, real FGSL shaders, a differential fuzzer, safety and negative tests. A path the host CPU lacks is skipped with a message |
| `nv_hw_tests` | NVIDIA firmware, VBIOS, and resource-manager data parsing |
| `hid_mouse_tests` | HID mouse report decoding |
| `media_tests` | Host-testable media interfaces |
| `os0_tests` | OS0 host helpers: time conversion, path resolution and the program argument block |

These are ordinary host binaries under `build/tests/host`. The Makefile uses
`HOSTCC` (default `gcc`) and `HOST_CFLAGS` for their compilation.

### Shader JIT checks

```sh
make test-host                       # includes fgpu_jit_tests (a few seconds)
make fuzz-fgpu-jit FUZZ_SECONDS=300  # longer differential fuzzing (FUZZ_SEED=n to change the seed)
make bench-fgpu-jit                  # interpreter vs SSE4.1 vs AVX2, lane-instructions/s (not part of test-host)
FGPU_JIT_DUMP=1 build/tests/host/fgpu_jit_tests --soak 20   # on a failure: shrunk program + listing
```

In QEMU (TCG), the JIT needs a CPU with SSE4.1 and AVX2: `-cpu max`; the
default `qemu64` CPU has neither, so the shader JIT stays off there.

```sh
QEMU_EXTRA="-cpu max -smp 4" bash tools/debug/ci_fgpu_jit.sh   # `gpu jit`: interpreter vs JIT in the kernel, also on helper cores
bash tools/debug/ci_fgpu_jit.sh                                 # default CPU: reports SKIP
GRUB_UP=3 QEMU_EXTRA="-cpu max" bash tools/debug/ci_fgpu_jit.sh # the nojit entry: reports SKIP
SHOT=/tmp/a.ppm QEMU_EXTRA="-cpu max -smp 4" bash tools/debug/ci_fgpu_jit_cube.sh   # fglcube --shader --still screendump
SHOT=/tmp/b.ppm GRUB_UP=3 QEMU_EXTRA="-cpu max -smp 4" bash tools/debug/ci_fgpu_jit_cube.sh && cmp /tmp/a.ppm /tmp/b.ppm
```

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

Two more variables select what is tested. `QEMU_EXTRA` is appended to the
QEMU command line (word split): QEMU's default CPU, `qemu64`, has no AVX, so
`QEMU_EXTRA="-cpu max"` is how the XSAVE/AVX path and the YMM torture test
run (`-cpu max -smp 4` adds cores; a later `-smp` wins over the script's
`-smp 2`). `GRUB_UP=1` presses `up` once after the last entry is selected,
which picks "Minimal-OS (RamFS, noavx)" (the entry above the RamFS one, so the
RamFS entry stays last). The configurations OS-2 was checked in:

```sh
bash tools/debug/ci_os0.sh                                   # qemu64: fxsave path
QEMU_EXTRA="-cpu max" bash tools/debug/ci_os0.sh             # xsave + AVX2
QEMU_EXTRA="-cpu max -smp 4" bash tools/debug/ci_os0.sh      # same on 4 cores
QEMU_EXTRA="-cpu max" GRUB_UP=1 bash tools/debug/ci_os0.sh   # noavx flag
```

### Binned rasterizer tests and benchmark

`fgpu_bin_tests` (`tests/host/fgpu_bin_tests.c`) renders random scenes through the binned
path (default) and through the immediate path (`fgpu_raster_set_mode`), with random
depth/blend/cull/viewport state, several draws, clears in between, indexed and plain
draws, the built-in programs and four FGSL shaders (one discards, one reads the
fragment coordinate and facing, one samples a texture):

- *exact scenes* (two thirds): screen vertices on a quarter-pixel grid, orthographic, so
  both paths evaluate edges exactly; color, depth and the triangle count must match bit
  for bit. They include degenerate and sliver triangles, tiny (2x2) ones, sample
  centers exactly on edges, quads with shared diagonals, huge triangles, off-screen
  parts, clipped viewports, bad indices.
- *general scenes*: perspective, near-plane clipping, huge triangles. Pixels may differ
  only at triangle borders (1/256 snapping); the share of differing pixels per scene is
  bounded (0.4%, and 2% for "hostile" scenes with slivers and triangles crossing the
  camera; the observed worst case over 7500 scenes is 0.16% and 0.29%).
- every scene is also rendered on several host threads and through a pass with random
  flush points and with the pipeline state, constants, layout and vertex data destroyed
  right after recording (snapshot semantics); the result must be identical to the single
  thread result.
- watertightness: shared-edge meshes with interior vertices on a 1/256 grid or on pixel
  centers (the top-left rule decides), blended with alpha 1/2: every pixel inside
  the outline is touched exactly once, none outside.
- a 1 MB bin budget with 30000 triangles (several rounds) and allocation failures every
  2nd/7th allocation must give the same picture.

`FGPU_FUZZ_ITERS=n` and `FGPU_FUZZ_SEED=s` change the run (default 600 scenes, seed 1);
`FGPU_FUZZ_DEBUG=1` prints the first differing pixels and the draws of a failing scene.
For memory errors build it with `-fsanitize=address,undefined`.

`make bench-fgpu` (not part of `test-host`) times the immediate and the binned path on a
640x480 target for 2000 draws of 12 triangles, 8 huge triangles, 200k triangles of about
1.2 px, and 32 layers of overdraw (front to back with hierarchical Z, and back to front),
with a built-in program and with a one-instruction FGSL shader, on 1 thread and on
`FGPU_BENCH_THREADS` (default 4) host threads standing in for the kernel's workers
(`fgpu_raster_set_parallel`). It reports triangles/s, pixels/s, the per-draw overhead
and the share of time spent inside `fgpu_shader_run_batch` (measured with a linker wrap).
`FGPU_BENCH_PHASES=1` adds the cycles spent in the geometry and the tile phase.

## FarcadeGL screenshot test (`ci_fglcube.sh`)

```sh
bash tools/debug/ci_fglcube.sh                      # builds, then boots twice
FGLCUBE_ARGS=--shader NO_BUILD=1 bash tools/debug/ci_fglcube.sh
```

Boots the ISO headless in RamFS mode with the binned rasterizer, runs
`fglcube 600 --still` from the terminal (typed over serial like `ci_os0.sh`; `--still`
fixes the animation at one instant and prints `FGLCUBE READY` after the third frame),
takes a screenshot with the QEMU monitor (`screendump`), then does the same with the
`fgpuold` GRUB entry (the previous rasterizer; `GRUB_UP=2`, the noavx entry stays at
`GRUB_UP=1`). It converts both to PNG in `dist/ci_fglcube/` (`new.png`, `old.png`,
serial logs next to them), checks that the pictures show a cube (at least 2% of the
screen differs from the background) and compares them: the default allowance is 2000
ppm of pixels (`MAX_DIFF_PPM`) because the two rasterizers differ at triangle borders
by the 1/256 pixel snapping. `ONLY=new` or `ONLY=old` takes one screenshot.

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
| FGPU rasterizer or `fgpu.c` flush points | `make test-host`, `make bench-fgpu`, `bash tools/debug/ci_fglcube.sh`, `bash tools/debug/ci_os0.sh` |
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
