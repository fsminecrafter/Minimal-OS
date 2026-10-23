# GPU and 3D

## Adapters and video memory

`gpu_core_init()` (after the display driver is up) gives every
display-class PCI device a `gpu_adapter_t` (`src/intf/x86_64/gpucore.h`):

- `vram_size` – total video memory,
- `aperture_*` – the PCI BAR through which the CPU sees VRAM, mapped
  write-combining,
- `scanout_*` – where the framebuffer the screen shows sits in VRAM,
- `heap_start/heap_end` – the rest of the window, handed out by
  `gpu_vram_alloc()`; it is only enabled after a write/read-back test.

`gpu` in the terminal prints all of this.

### NVIDIA (GTX 10 "Pascal", RTX 20 / GTX 16 "Turing")

`gpu_nvidia.c` reads the chip from `PMC_BOOT_0`, the board name from the
PCI device id (GT 1030, GTX 1050/1050 Ti/1060/1070/1080, RTX 2060/2070/
2080 and their variants, GTX 1650/1660), the VRAM size from the memory
controller (register `0x100CE0`, as nouveau does for Pascal through
Turing) and the core temperature. BAR1 is the VRAM window; it is only used
for allocations while BAR1 is in physical mode, which is how the firmware
leaves it.

The display mode is the one the firmware set (GOP/VBE). FGPU work is
rendered on the CPU into VRAM; see
[NVIDIA cards and hardware rendering](#nvidia-cards-and-hardware-rendering)
for what running it on the card itself takes.

### QEMU / Bochs

`gpu_bochs.c` reads the VRAM size and can page flip (show any line of
VRAM). Because VRAM is ordinary host memory there, the kernel's screen
back buffer is moved into VRAM as well. Give QEMU more VRAM with
`-vga none -device VGA,vgamem_mb=64` (`make run` does).

## FGPU command processor

`SYS_GPU` (40) is the GPU interface for programs (`fgpu_abi.h`, shared
word for word with the SDK). A process creates *surfaces* (color
XRGB8888, depth float32), *buffers* (vertices, indices) and *shader
programs*, records a command buffer (bind target, viewport, clear, state,
program or shader, constants, vertex layout, textures, draw) and submits
it; then presents a color surface to the screen (copy, or page flip when
the surface is in VRAM and the adapter can flip) or into a window.
Everything is freed when the process exits.

ABI version 2 added shader programs (`CREATE_PROGRAM`,
`DESTROY_PROGRAM`), texture uploads (`WRITE_SURFACE`) and the commands
`SET_SHADER`, `SET_CONSTANTS`, `SET_VERTEX_LAYOUT` and `BIND_TEXTURE`,
plus the `FGPU_STATE_BLEND` state. The kernel still accepts version-1
requests, so older programs keep working.

### CPU backend

`fgpu_raster.c` runs the pipeline: vertex stage, near-plane clipping,
perspective-correct rasterization with the top-left rule, depth test,
back/front face culling, wireframe, fragment stage and alpha blending.
The built-in programs (vertex color, Gouraud, per-pixel Phong with
Blinn-Phong highlights) are hard-wired C code.

### Shader machine

Shaders are FGPU bytecode (described at the end of `fgpu_abi.h`): vec4
registers in a few files (temporaries, inputs, outputs, uniforms,
constants, fragment specials), five-word instructions with swizzles,
write masks and negate/abs modifiers, and about forty opcodes (arithmetic,
dot products, `pow`/`exp2`/`log2`/`sin`/`cos`, `mix`/`clamp`/
`smoothstep`, comparisons and select, texture sampling, discard, jumps).
The SDK's FGSL compiler (`fglsl.h`) produces it from GLSL-style source.

`fgpu_shader.c`:

- `fgpu_program_check` validates a program once, on the kernel's own copy:
  every opcode, register index, write mask, texture unit and section
  bound. **Jumps may only go forward**, so a program runs at most its
  instruction count (2048 per stage) and cannot hang the kernel. Rejected
  programs are logged to serial (`fgpu: shader program rejected: ...`).
- The interpreter runs 8 invocations at once, like a GPU warp: registers
  are stored per component as 8-lane rows and the lane loops use SSE
  (GCC vector extensions). Branches are handled per lane with a
  "resume at" instruction: lanes that jump wait until execution reaches
  their target, so divergent `if`/`else` runs correctly. Temporaries are
  zeroed for every draw, so a shader never sees other data.
- The rasterizer queues covered pixels and shades them 8 at a time, and
  runs each triangle's three vertices as one batch. A shader that can
  `discard` writes depth after the fragment stage instead of before it.

`make test-host` runs the compiler, checker, machine and rasterizer end to
end (`tests/host/fgsl_tests.c`), including a check that a shader version
of a built-in program gives the same pixels.

Speed: the `fglcube` scene at 640 × 480 on one 2.8 GHz core takes about
11 ms per frame with the built-in Phong program and about 42 ms with
`fglcube --shader` (a 60-instruction fragment shader over about 180,000
pixels).

## FarcadeGL

The SDK's `farcadegl.h` wraps all of this in an OpenGL/Vulkan-like API
with GLSL-style shaders. Guides:
[farcadegl.md](farcadegl.md) (API) and [fgsl.md](fgsl.md) (shading
language), also in `Minimal-OS-SDK/docs/`. `fglcube` (terminal, full
screen; `fglcube --shader` for the shader version) and MOSdesk's *Cube*
app (press S) are examples.

## NVIDIA cards and hardware rendering

Today FGPU renders on the CPU on every adapter. Running it on the
graphics card's own engines is the next step, and on NVIDIA cards it is
possible with open code. This section explains what stands in the way and
how open drivers get around it.

### What has to be signed

Since the GTX 900 series, an NVIDIA GPU only lets **firmware signed by
NVIDIA** run on its internal microcontrollers ("falcons", and on RTX 20
and later the GSP, a RISC-V core). That firmware sets up the graphics
engine's context-switching units (FECS/GPCCS), power management and
memory protection; without it the graphics engine does not run.

What does *not* have to be signed:

- the **command streams** (pushbuffers) a driver sends to a channel:
  "set this register of the 3D class, draw these vertices";
- the **shader programs** the shader cores (SMs) execute.

Both are written by the driver. So an open driver needs NVIDIA's firmware
files, but nothing it produces itself has to be signed.

### How open drivers do it

nouveau (Linux) and NVIDIA's own open kernel modules load **NVIDIA's
signed firmware files**, which NVIDIA publishes for redistribution in the
`linux-firmware` repository (`nvidia/gp10x/...`, `nvidia/tu10x/...`,
under NVIDIA's firmware licence, `LICENCE.nvidia`). Minimal-OS can do the
same: ship those files unchanged on the system volume and load them at
boot. Then:

| | GTX 10 (Pascal) | RTX 20 / GTX 16 (Turing) |
| --- | --- | --- |
| Firmware | ACR + graphics (FECS/GPCCS) + SEC2 images from `linux-firmware`, booted through the secure-boot falcon as nouveau does | The GSP firmware (`gsp-*.bin`) plus its boot loader; GSP then sets up most of the chip itself |
| Driver talks to | Registers directly | GSP through a message queue (RPC), as nouveau's GSP mode and NVIDIA's open kernel modules do |
| Clock speeds | Stays at boot clocks: NVIDIA never released the PMU firmware needed to raise them, so 3D runs much slower than the card's real speed | Full speed (GSP manages clocks) |
| Commands | Channels with a GPFIFO of pushbuffers; 3D class `PASCAL_A`/`PASCAL_B` | Same, 3D class `TURING_A` |

The class methods (register meanings) are published by NVIDIA in
`open-gpu-doc`. The shader instruction set (SASS) is not officially
documented, but Mesa's open compilers (the nouveau codegen and NAK, used
by the NVK Vulkan driver) generate it, and `envytools` documents much of
it.

### The plan for Minimal-OS

1. **Firmware loading**: read the `linux-firmware` files from
   `0:/firmware/nvidia/`, check them, and boot GSP on Turing (simpler and
   full speed) or ACR on Pascal.
2. **Channels**: allocate a channel, a GPFIFO and pushbuffers in VRAM,
   and submit a "clear the screen" through the 3D class to prove the path.
3. **An FGPU hardware backend**: translate FGPU command buffers to
   pushbuffers (surfaces → render targets, `DRAW` → vertex buffer setup
   and draw methods) and **FGPU shader bytecode to SASS**. The bytecode
   was designed for this: straight-line code with forward jumps, vec4
   registers and a small instruction set map well onto a GPU compiler.
4. Programs do not change: FarcadeGL and FGSL stay the same, and
   `syscall_gpu_info_t.backend` reports the hardware backend.

This needs testing on real cards (QEMU cannot emulate an NVIDIA GPU);
steps 1-2 are the first milestone.

### What is in place (Turing, e.g. RTX 2060)

Everything below builds into the kernel and changes nothing until the
hardware backend's `init()` succeeds; until then all work runs on the CPU
as before.

| Piece | Where | State |
| --- | --- | --- |
| Backend interface: FGPU offers every clear and draw to a hardware backend first and falls back to the CPU per command; GPU work is finished before the CPU touches a surface or buffer | `fgpu_backend.h`, `fgpu.c` | done |
| NVIDIA backend: chosen at boot for NVIDIA adapters; compiles every shader program to the IR below; declines `init()` with the reason | `fgpu_backend_nv.c` | stub |
| Shader compiler, first half: FGPU bytecode → scalar SSA IR (one op per hardware instruction), if-conversion of jumps, constant folding, CSE, dead code removal, linear-scan register allocation, SASS-style listing | `fgpu_ir.h`, `fgpu_ir.c` | done, tested |
| Shader compiler, second half: IR → encoded Turing SASS (128-bit instructions with scheduling bits) | | to do |
| Firmware: list of files per chip, parsers for NVIDIA's `.bin` containers and the GSP-RM ELF image | `nv_hw.h`, `nv_fw.c` | done, tested |
| Falcons: read state; IMEM/DMEM upload, start, wait | `nv_falcon.c` | reading used, the rest ready for GSP boot |
| Pushbuffers: method headers, GPFIFO entries, a ready-made `TURING_A` clear | `nv_fw.c` | done, tested (method offsets unverified on hardware) |
| GSP boot (FWSEC-FRTS from the VBIOS, `booter_load` on SEC2, GSP-RM) | | to do |
| RM RPC: channel, GPFIFO and class allocation through GSP-RM | | to do |

`gpu nvidia` prints the bring-up state. It only reads registers, so it is
safe on any card: which firmware files are present and well-formed, and
each microcontroller (GSP, SEC2, PMU, FECS, GPCCS, NVDEC) with its memory
sizes and whether it is running.

#### Firmware files

`tools/gpu/setup-gpu.sh` (or `make gpu-setup`) sets them up:

```sh
make gpu-setup                      # RTX 2060 (tu106), the default
make gpu-setup GPU="GTX 1660 Ti"    # another card, a chip (GPU=tu104)
                                    # or a PCI id (GPU=10de:1f08)
make gpu-setup GPU=--detect         # the NVIDIA card in the build PC
make mosdesk                        # or make / make run-mosdesk
make gpu-remove                     # build without firmware again
```

It copies the files from the build PC's `/lib/firmware` (unpacking
`.zst`/`.xz`), or downloads them from linux-firmware on git.kernel.org,
checks them, and puts them in `firmware/nvidia/<chip>/` (ignored by git;
NVIDIA's licence goes along as `firmware/nvidia/LICENCE.nvidia`). Every
build then packs `firmware/` into the ISO as `boot/mosfirmware.mpkg`.
GRUB loads it as the `mosfirmware` module when it is there, and the kernel
unpacks it into `0:/firmware` whenever it sets up a system volume: RamFS
boot (which then gets a 192 MiB RAM disk), `format` and `install`. A disk
formatted before the firmware was added does not get it; format it again
or boot RamFS.

The files, by hand: copy these from `linux-firmware` (`nvidia/tu106/` for
an RTX 2060; follow the symbolic links, MinimaFS has none) to
`0:/firmware/nvidia/tu106/`:

```text
gsp/booter_load-535.113.01.bin
gsp/booter_unload-535.113.01.bin
gsp/bootloader-535.113.01.bin
gsp/gsp-535.113.01.bin          (about 30 MB)
```

TU102/TU104 use `tu102`/`tu104`, the GTX 16 series `tu116`/`tu117`.

#### The shader IR

GPU shader cores run 32 threads in step, each on scalar 32-bit registers,
so `fgpu_ir.c` turns each vec4 instruction into scalar operations that
correspond one to one to Turing instructions (`FFMA`, `FMNMX`,
`MUFU.RSQ`, `FSET.BF`, `FSEL`, `TEX`, `IPA`, ...):

- the edge cases FGPU defines (`x / 0 = 0`, `sqrt`/`rsq` of `x <= 0`,
  `log2(x <= 0) = -126`, `pow` of `x <= 0`) become explicit selects, so GPU
  and CPU agree;
- `atan2`, `smoothstep`, `mod` and `sign` become short instruction
  sequences (the same polynomial as the CPU backend);
- jumps are if-converted: each thread keeps the instruction it resumes at,
  as the CPU interpreter does per lane, and writes inside a skipped range
  become selects. The result has no branches at all; turning long skipped
  ranges back into real branches (`BRA` with `BSSY`/`BSYNC`) is a later
  optimization.

`make test-host` compiles FGSL shaders (lighting, every built-in function,
nested `if`/`else` with `discard`, loops with `break`/`continue`,
textures, `gl_FragCoord`) and hand-written bytecode with divergent jumps,
runs them on random inputs through both the IR (with a reference
evaluator) and the CPU shader machine, and checks that they agree.
`FGPU_IR_LIST=1 ./build/tests/host/fgpu_ir_tests` prints a listing:

```text
// fragment stage: 35 ops, 7 registers
  IPA R0, v3.x
  IPA R1, v3.y
  TEX.2D R0+, R0, R1, tex0
  FSET.BF.LT R3, R3, 0.5
  ...
  KIL if R3 != 0
```

#### Next steps on a card

1. Run `gpu nvidia` with the firmware copied: every file should show
   `ok`, and GSP/SEC2 should be reachable.
2. GSP boot, following nouveau's `r535` code (`nvkm/subdev/gsp/r535.c`,
   `tu102.c`): run FWSEC-FRTS from the VBIOS, load `booter_load` on SEC2
   with the GSP-RM image and boot loader in system memory, and wait for
   GSP-RM's init message.
3. Allocate a channel through RM RPC, submit `nv_push_clear_xrgb` and
   check the surface changed: then `clear()` in `fgpu_backend_nv.c` can
   return true.
4. Encode the IR as SASS (Mesa's NAK, `src/nouveau/compiler/nak/sm70.rs`,
   has the Volta/Turing encodings), then draws.
