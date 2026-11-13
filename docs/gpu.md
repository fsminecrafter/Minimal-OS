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

#### Binned rasterizer (default)

Draws are not drawn when they are submitted. `fgpu_raster.c` records them in
a pass (`fgpu_pass_t`: one render target at a time) with a snapshot of
everything they depend on, and renders the pass at a flush:

1. **Record.** A draw stores its pipeline state, uniforms, shader constants and
   vertex layout (copied, and deduplicated against the last 8 states), the
   viewport clipped to the target, and pointers to its vertex and index data.
   A full clear at the start of a pass is not executed: it is remembered
   and merged into the tile stage. Lines (`FGPU_STATE_WIREFRAME`) and draws
   that sample the surface they draw into are not recorded; they flush the
   pass and run through the immediate path, so submission order holds.
2. **Geometry** (one parallel phase). Workers take *items* (up to 32
   triangles of one draw) from an atomic counter. An item shades its vertices
   with all 8 shader lanes full (built-in programs run a cut-down vertex stage
   that computes only what the fragment stage reads), clips against the near
   plane (and, for triangles that reach beyond +-32768 px, against a
   guard band in screen space), culls, transforms, snaps the vertices to
   **16.8 fixed point** and sets up integer edge functions (top-left rule, exact
   and independent of the order the pixels are visited in). The triangle record
   and its bin entries go into the worker's arena and are added to the bin list
   of every 64x64 tile the triangle may cover (long thin triangles are tested
   against each tile). Lists are per worker and per tile.
3. **Tiles** (one parallel phase). Workers take tiles from an atomic counter.
   A tile merges the per-worker lists by item number, which restores the
   submission order exactly (blending and depth-equal results are the same as
   one core drawing in order), loads color and depth into a local 64x64 copy
   (or fills it from the pending clear: a tile that only needs the clear is
   written straight to the target), rasterizes, and stores the copy back.
   Triangles with a bounding box of at most 400 pixels (this includes the
   2x2-pixel case) evaluate their edge functions directly at the sample
   centers. Bigger ones walk 8x8 blocks: a block is rejected, or accepted without
   per-pixel coverage tests, from the edge functions at its corners, and
   with depth testing a block is also rejected when the triangle's nearest depth
   in the block is behind the farthest depth stored there (**hierarchical Z**,
   min/max per block, kept conservative as pixels are written) or its depth
   test is skipped when it is entirely in front. A tile entirely inside a
   triangle skips the coverage tests.
4. Fragments of consecutive triangles with the same state are shaded in
   batches of 8 lanes (the shader interface is unchanged); shaders that can
   `discard` flush per triangle so a later triangle sees the final depth.

**Memory.** Records and bins come from the heap (`fgpu_raster_set_allocator`; the
kernel passes `alloc_unzeroed`/`free_mem`) in 64 KB chunks that the main thread
allocates before each phase (nothing allocates inside a parallel phase). Bin
memory is bounded by a budget (8 MB, `fgpu_raster_set_bin_budget`). The geometry
stage runs in *rounds*; when a round does not fit, the items that did are
rasterized and the rest goes to the next round. An allocation failure doubles the
pool if the budget allows, and as a last resort an item is drawn by the immediate
path: a draw never fails because bins are full.

**Flush points.** The rasterizer flushes by itself when the target changes
(`fgpu_pass_clear/draw` with another color or depth buffer), when a clear comes
after draws, when a draw is a line or reads its own target, when 65536 draws or
2^24 items are pending, and when memory for the record runs out. The kernel
(`fgpu.c`) flushes at the end of every `SYS_GPU_SUBMIT` (so the triangle count it
returns is exact and nothing is pending between system calls), before the hardware
backend gets a clear or a draw, at the start of every other `SYS_GPU` operation
(readback, present, buffer upload, surface or buffer or program create and destroy,
`WRITE_SURFACE`), and `fgpu_process_exit` drops what a dead process left pending.
Pipeline state, uniforms and constants are snapshotted, so changing them between
draws needs no flush; vertex buffers, index buffers, textures and shader programs are
referenced, which is why every operation that writes or frees them flushes first.
`FGPU_DRAW_COPY` copies vertex and index data at record time for callers whose
memory does not stay put.

**Results.** Coverage, depth tests, interpolated values and blending order are
bit-identical to the immediate path whenever the immediate path evaluates its float
edge functions exactly, which is the case when vertices sit on a coarse grid (the
differential test generates such scenes). Otherwise they differ at the borders of
triangles: the immediate path computes edges in float with an accumulating row
step, and the binned path snaps vertices to 1/256 pixel and computes edges exactly,
so a sample very close to an edge can fall on the other side, and interpolated depth
and attributes move by the snapping (a few parts in 1e5; for slivers more). The
binned path is the more accurate of the two (the shared-edge meshes of the
watertightness test are covered exactly once; the immediate path has no such
guarantee). Vertices are clamped as before.

**Mode switch.** `fgpu_raster_set_mode(FGPU_RASTER_IMMEDIATE)` selects the previous
rasterizer (one barrier per draw, scalar); the kernel does this with the `fgpuold`
boot flag (GRUB entry "Minimal-OS (RamFS, fgpuold)", two above the last entry).
The host benchmark (`make bench-fgpu`) and the differential tests use the switch.

**Not done yet (OS-4).** Preemptible kernel worker threads instead of `smp_par`,
per-process contexts instead of `g_busy`, and asynchronous submit with fences. The
pass is per target and everything it keeps (workers' scratch, chunk pool, bin lists)
sits in the pass, not in globals, so a context per process can own one pass; the
kernel currently has a single pass guarded by `g_busy`. Deferring draws across
submits only needs the flush points above to stay (they are already called), plus
an exact triangle count some other way.

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

FGPU draws - and so FarcadeGL - go to the card's 3D engine once the
backend's own test passes (step 4b below): `gpu` then shows `hw-3d` and
the backend `nvidia 3D engine`. Until then clears use the copy engine and
draws the CPU, as before. Steps 4a and 4b are written from NVIDIA's
published class headers and Mesa's NVK/NAK and checked on the build PC,
but **have not run on a card yet**; the test and its log lines exist to
find out what the hardware disagrees with.

| Piece | Where | State |
| --- | --- | --- |
| Backend interface: FGPU offers every clear and draw to a hardware backend first and falls back to the CPU per command; GPU work is finished before the CPU touches a surface or buffer | `fgpu_backend.h`, `fgpu.c` | done |
| NVIDIA backend: shader programs compiled to machine code and uploaded, built-in programs from FGSL, a GPU depth buffer per depth surface, clears and draws as 3D jobs, a draw self-test | `fgpu_backend_nv.c` | written, not yet run on a card |
| Shader compiler, first half: FGPU bytecode → scalar SSA IR (one op per hardware instruction), if-conversion of jumps, constant folding, CSE, dead code removal, linear-scan register allocation, SASS-style listing | `fgpu_ir.h`, `fgpu_ir.c` | done, tested |
| Shader compiler, second half: IR → encoded Turing SASS (128-bit instructions with scheduling bits) and the shader program header | `nv_sass.h`, `nv_sass.c` | done; tested against the CPU shader machine with a simulator, not yet on a card |
| 3D pushbuffers: color and depth targets, viewport, depth/cull/blend state, vertex streams and attributes, shader stages, constant buffer, texture headers and samplers, draw | `nv_3d.h`, `nv_3d.c` | done, tested on the build PC, not yet on a card |
| Firmware: list of files per chip, parsers for NVIDIA's `.bin` containers and the GSP-RM ELF image | `nv_hw.h`, `nv_fw.c` | done, tested |
| Falcons: read state; IMEM/DMEM upload, start, wait | `nv_falcon.c` | reading used, the rest ready for GSP boot |
| Pushbuffers: method headers, GPFIFO entries, a ready-made `TURING_A` clear | `nv_fw.c` | done, tested (method offsets unverified on hardware) |
| VBIOS: ROM image chain, BIT table, falcon ucode table, FWSEC's descriptor and DMEM mapper (where the FRTS command goes), read through the BAR0 PROM window | `nv_vbios.c` | done, tested on a synthetic ROM, not yet on a card |
| GSP boot: FWSEC-FRTS | `nv_gsp.c` | works on an RTX 2060 |
| GSP boot: `booter_load` on SEC2, GSP-RM (WPR meta, radix3 image, LibOS args, message queues, CPU sequencer) | `nv_rm.h`, `nv_rm_fmt.c`, `nv_gsp_rm.c` | works on an RTX 2060 |
| RM RPC: first round trip (`GET_GSP_STATIC_INFO`) | `nv_gsp_rm.c` | works on an RTX 2060 |
| RM RPC: client, device, subdevice, VA space; FIFO device info; channel on a copy engine (TURING_CHANNEL_GPFIFO_A + TURING_DMA_COPY_A) | `nv_rm_chan.c`, `nv_rm.h` | works on an RTX 2060 (channel 0 on CE2) |
| GPU page tables (GMMU v2, in VRAM, written through BAR1): sysmem GPFIFO/pushbuffer/semaphore, all of the BAR1 window's VRAM 1:1 in 2 MiB pages; GSP-RM gets the top three levels (COPY_SERVER_RESERVED_PDES) | `nv_rm_chan.c` | works on an RTX 2060 |
| Copy engine fills: test pattern on screen, FGPU clears of VRAM surfaces | `nv_push_ce_fill`, `nv_rm_ce_fill`, `fgpu_backend_nv.c` | works on an RTX 2060 (test squares read back right) |
| 3D class (TURING_A): graphics context buffers, golden context, 3D channel, first clear | `nv_rm_chan.c` | written, not yet run on a card |
| Video decoder (NVDEC, `NVC4B0_VIDEO_DECODER`): channel, H.264 picture setup (`nvdec_h264_pic_s`) and decode jobs, NV12 to RGB on the 3D engine | `nv_nvdec.h`, `nv_nvdec.c`, `nv_rm_chan.c`, `fgpu_backend_nv.c` | written, not yet run on a card |
| Readback: the copy engine copies VRAM to staging memory in system RAM (16 MiB, mapped once), so presenting a frame never reads through BAR1 | `nv_rm_ce_read` | written, not yet run on a card |

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
   `ok`, GSP/SEC2 should be reachable, and step 2a should list the ROM
   images and end with `ok: FWSEC can be patched with the FRTS command`.
2. `gpu nvidia boot`: FWSEC-FRTS (works), then `booter_load` on SEC2
   and GSP-RM, following nouveau's `r535` code (`nvkm/subdev/gsp/r535.c`,
   `tu102.c`), then a first RPC (step 3a). Everything it prints is also
   saved to `0:/gsp_boot.log`, in case the screen goes dark. What to look
   for: `booter_load halted, mailbox 0x0 0x0` (non-zero mailbox 0 is
   booter's error code), `GSP RISC-V running`, then `GSP-RM is running`
   and the GPU's name from the first RPC.
   First run on an RTX 2060 (2026-11-06): booter_load worked, GSP-RM's
   init task logged two entries (the second right after mapping RMARGS)
   and stopped with GSP mailbox `0xCA`. Cause: the RMARGS region was 0x50
   bytes; LibOS's region mapper (init task, ~0x568de1c) asserts that sizes
   are whole 4 KiB pages, logs the address and size, and panics (its
   panic routine at 0x402521c writes `0xCA` to mailbox 0 and halts).
   RMARGS is now one page, as in nouveau. Mailbox `0xCA` in general means
   a LibOS crash; the last LOGINIT/LOGRM entries say where.
   Next run: GSP-RM booted, read both queued RPCs and sent
   RUN_CPU_SEQUENCER; our CORE_RESUME reset the GSP core before
   restarting booter, which killed it. Now it only rewrites the GSP
   mailboxes and restarts SEC2, as nouveau does.
3. `gpu nvidia boot` continues on its own once GSP-RM answers:
   step 3b allocates the RM objects, builds the page tables and opens a
   channel on an async copy engine (it lists the copy engines it found
   and which one took the channel), submits a first job and reads it
   back. Step 3c draws four squares (red, green, blue, white) in the
   top right corner of the screen with the copy engine and reads them
   back through BAR1. Then FGPU sends clears of surfaces in VRAM to the
   copy engine. Failure lines to look for: `RM error 0x..` (with the
   object or control that failed), `no semaphore after 2000 ms` (with
   GP_GET/GP_PUT: GP_GET still 0 means the doorbell or the channel
   setup, GP_GET moved means the copy engine or its page tables),
   `channel error (RC)` and `GPU page fault` from GSP-RM. A failed job
   switches the GPU path off again, so the desktop keeps working.
   First run (2026-11-06): everything up to the channel worked (CE2,
   runlist 8), but the job's USERD read back `0xBAD0ACC5` and the
   desktop froze: GSP-RM switches BAR1 into virtual mode with its own
   page tables while it starts (nouveau adopts them, `bar/r535.c`), so
   the kernel's 1:1 BAR1 window onto VRAM - the screen, the VRAM heap,
   our page tables - stopped working. `nv_bar1_keep_physical` puts BAR1
   back into physical mode (0xB80F40 bit 31) after GSP-RM starts and
   before each job, and says so (`BAR1: ... back to physical`).
   Second run: the first job and the four test squares read back right,
   and FGPU switched its VRAM clears to the copy engine.
4. `gpu nvidia boot` goes on by itself. **Step 4a** (3D channel) prints
   `golden context recorded`, the channel, and `first 3D clear: reads
   back 0xffff8000 (ok)`. **Step 4b** compiles the built-in shader, then
   clears a 64x64 surface and draws two triangles with depth test and
   back-face culling, and prints three pixels:

   ```text
   Step 4b: FarcadeGL draws on the 3D engine
     shaders: vertex and pixel stage compiled (19 + 13 registers)
     clockwise front: centre 0xff4080bf, left 0xffff0000, corner 0xff102030
     ok: triangles, shaders, depth test and culling work on the 3D engine.
   ```

   The centre is inside both triangles (the near, blue-ish one must win),
   `left` only inside the far red one, `corner` in neither. If the first
   try shows only the clear color, the test repeats with the other
   winding as "front" and keeps what works. What other results mean:

   | Result | Look at |
   | --- | --- |
   | `the test draw did not finish`, with `channel error (RC)` or `GPU page fault` above it | a method or address the engine rejects: `nv3d_push_init` / `nv3d_push_draw` in `nv_3d.c` (the error names the method offset) |
   | all three pixels are the clear color with both windings | nothing was rasterized: vertex stream/attribute setup, the vertex stage's code or header (`nv_sass.c`), or the viewport |
   | centre and left have a color, but the wrong one | the pixel stage: interpolation (`IPA`), the output registers, or the constant buffer |
   | centre is red | the depth buffer: block-linear layout or depth state |

   After `ok`, FGPU puts new color surfaces, textures and buffers in VRAM
   and sends clears and draws to the 3D engine. `gpu nvidia` shows how
   many ran. A job that does not finish later switches the 3D path off
   and rendering continues on the CPU.

5. **Step 5a** (video decoder): a channel on NVDEC with the decoder
   class (`NVC4B0_VIDEO_DECODER`); GSP-RM runs NVDEC's firmware itself.
   It prints `channel N on NVDEC (runlist R)` and `ok: the video
   decoder's channel runs.` after an empty first job. `gpu nvidia` then
   shows step 5 with the number of H.264 pictures NVDEC decoded, or that
   it was switched off because its pictures differed from the CPU
   decoder's (see docs/media.md for how files are checked). A failed
   decode job switches NVDEC off and H.264 goes on on the CPU.

#### How a draw runs on the card

- **Shaders.** `nv_sass.c` turns each IR operation into one to three
  instructions (immediates and operands with modifiers the instruction
  cannot take go through scratch registers), in "serial" scheduling:
  every instruction waits for the one before it, which is slow for a GPU
  and always right. Uniforms are constant buffer 0, loaded with every
  draw; texture handles sit behind them. The vertex stage writes every
  varying and the pixel stage's header lists every varying as read, so
  the two always agree. Not encoded yet: front-facing (`gl_FrontFacing`);
  such a program stays on the CPU.
- **Built-in programs** (color, Gouraud, Phong) are FGSL source in
  `nv_builtin_fgsl.h`, compiled in the kernel with the SDK's `fglsl.h`
  the first time they are drawn.
- **Memory.** Color targets are pitch-linear in VRAM, pitch a multiple of
  128 bytes (FGPU pads surfaces to 32 pixels when draws run on the card).
  Depth is a block-linear ZF32 buffer of the backend's own per depth
  surface, because the hardware has no pitch-linear depth. The CPU depth
  buffer only follows the clears, so a frame that mixes GPU draws with a
  draw that must fall back to the CPU (a refused shader, memory outside
  VRAM, a full-screen page-flip surface whose pitch does not fit) does
  not depth-test between the two.
- **One job per clear or draw**, submitted and waited for. Batching a
  frame into one job is the obvious next speed-up.
- **Reading back.** `fglPresent` into a window and `fglReadPixels` copy
  the frame out of VRAM with the copy engine (`nv_rm_ce_read`).

`make test-host` runs `tests/host/nv_sass_tests.c`: it decodes the
machine code with a small simulator and compares every output with the
CPU shader machine on random inputs (built-in programs, textures with
`discard`, math and branches), and checks the pushbuffers word by word.
That proves the encoder against the IR; only a card can prove the
encodings themselves.
