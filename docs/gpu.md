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

ABI version 3 adds asynchronous submits. `SUBMIT` with
`FGPU_SUBMIT_ASYNC` copies the command buffer, queues it and returns at
once with a fence in `handle` (increasing, never 0); `SYS_GPU_WAIT`
(handle = fence, 0 for all; bytes = timeout in ns, 0 to poll, ~0 forever)
waits for it, returns the triangles drawn by async submits since the last
`WAIT` in `handle`, and `SYS_ERR_INVAL` once when one of those buffers was
rejected. A program's queued buffers run in order, and every other call of
that program (readback, present, uploads, destroys, a plain `SUBMIT`)
first waits for them, so results are always seen. At most 4 are queued
per program; a fifth `SUBMIT` waits for room. `FGPU_CAP_ASYNC` in
`SYS_GPU_INFO` says the kernel has this. Version-2 requests ignore the
flag. A kernel process, `fgpu-worker`, runs the queued buffers; it is
preemptible like any process.

ABI version 4 (OS-6, first part) adds the scissor test, mipmapped
textures with trilinear filtering and 16 texture units, all drawn by the
CPU backend whatever the adapter:

- `FGPU_OP_SCISSOR` (enable, x, y, w, h): draws and clears only touch the
  rectangle, clipped to the target. The origin is the top-left pixel (y
  down), like `FGPU_OP_VIEWPORT`. The state belongs to the command buffer
  (every `SUBMIT` starts with it off). See "Scissor" below.
- `FGPU_SURFACE_MIPMAPS` (create flag, color only): the surface is
  allocated with a whole mip chain after level 0 (level k is max(1, w >> k)
  by max(1, h >> k), tightly packed, `fgpu_mip_layout`), in system memory.
  `WRITE_SURFACE` takes the level in `offset` (version 4 requests only);
  `FGPU_OP_GENERATE_MIPMAPS` rebuilds levels 1.. with a 2x2 box filter in
  command order (pending draws land first). `BIND_TEXTURE` flags
  `FGPU_TEXTURE_MIP_NEAREST` / `FGPU_TEXTURE_MIP_LINEAR` select the filter and
  three optional words add a level-of-detail bias and min/max clamps. See
  "Mipmaps and quads" below.
- `FGPU_TEXTURE_UNITS` (16) texture units. `FGPU_MAX_TEXTURE_UNITS` stays at
  4: it is the number of units the NVIDIA backend can sample (its constant
  buffer and texture pools are laid out for four), and draws that use a
  higher unit or a mip chain are not offered to a hardware backend
  (`fgpu.c`, and a second guard in `nv_draw`, both marked `TODO(nv)`). The
  scissor reaches the NVIDIA backend: draws set the hardware's scissor 0,
  clears get the rectangle in `fgpu_hw_target_t` and use
  `SET_CLEAR_SURFACE_CONTROL.USE_SCISSOR0` on the 3D engine, or fill just
  the rectangle on the copy engine.
  Shaders that sample a unit above 3 do not compile for the NVIDIA backend
  either, so they run on the CPU.
- `SYS_GPU_INFO` accepts the shorter structure of version 3
  (`SYS_GPU_INFO_SIZE_BASE`) and the full one, which ends with a limits block
  (`max_texture_units`, `max_mip_levels`, `max_varyings`, `max_uniform_vec4`,
  `max_storage_buffers`, `max_surface_size`, `max_buffer_bytes`). Capability
  bits: `FGPU_CAP_SCISSOR`, `FGPU_CAP_MIPMAPS`, `FGPU_CAP_TEXTURE_UNITS16`.

**Objects and contexts.** Objects belong to a program, not a thread: all
threads of a program share its surfaces, buffers and shaders, and a thread
that ends frees nothing. They live in one table per type (grown on
demand); a handle encodes slot, type and a generation, so a stale or
wrong-type handle is not found. Each program may hold 1024 surfaces, 4096
buffers and 1024 shader programs. Each program has a context: a lock (its
calls and queued buffers run one at a time), its counts, its fences.
Drawing and presenting share one render lock (the CPU rasterizer keeps one
pass and per-worker scratch). With the CPU backend alone, creating,
uploading, reading back and freeing need only the program's own lock, so
they overlap another program's rendering; with a hardware backend or VRAM
they take the render lock too. A thread killed in the middle of a GPU call
leaves its locks to the next waiter, and its recorded draws are dropped.
When a program ends, its queued buffers are dropped and its objects freed
then, or by the worker or the next GPU call if a lock is busy at that
moment.

### CPU backend

`fgpu_raster.c` runs the pipeline: vertex stage, near-plane clipping,
perspective-correct rasterization with the top-left rule, depth test,
back/front face culling, wireframe, fragment stage and alpha blending.
The built-in programs (vertex color, Gouraud, per-pixel Phong with
Blinn-Phong highlights) are hard-wired C code.

#### Scissor

The rectangle (`fgpu_pipeline_t.scissor, sc_x .. sc_h`) only shrinks the
area a draw can touch: the binned path intersects it with the viewport and
the target when it records the draw (`bstate_t.cx0..cy1`), so triangle setup
clips its bounding box to it, the tile lists only hold the tiles it reaches
and no pixel test is added; the immediate path (`fgpuold`) intersects its
clip rectangle, its bands and the wireframe line plotter the same way. A
clear with a scissor cannot be merged into a pass (that merge is for
whole-target clears): `fgpu_pass_clear_rect` renders what is pending and then
clears the rectangle in parallel bands. A full-target rectangle takes the
normal merged path. `host tests`: `fgpu_bin_tests` checks, on exact and
general scenes, that pixels outside the rectangle keep their contents, pixels
inside equal the unscissored picture, and the immediate and binned paths
agree on any worker count.

#### Mipmaps and quads

`fgpu_texture_t` carries the chain (`mip_count`, `mip_offset[]`, bias and
clamps). `fgpu_texture_sample()` is unchanged (level 0, the no-mip path costs
what it did); `fgpu_texture_sample_lod()` picks the level (`MIP_NEAREST`:
nearest; `MIP_LINEAR`: two levels blended; the in-level filter is the `LINEAR`
flag, so `LINEAR | MIP_LINEAR` is trilinear; `lod <= 0` is magnification and
uses level 0).

The level of detail needs screen-space derivatives of whatever coordinate the
shader computes, so a fragment stage that samples a texture with a chain and a
mip flag runs on **2x2 pixel quads** (`fgpu_shader_batch_t.quad` = 1; lane 4q +
2 * row + column; `shader_uses_mips()` decides per draw, everything else keeps
one pixel per lane). The rasterizer collects the pixels a triangle covers
(early depth test and depth writes as before), then shades every touched quad
with all four pixels: pixels the triangle does not cover (or that lie outside
the clip rectangle, the band or the tile) are *helper* pixels whose varyings
come from the triangle's plane equations at their centers; they are shaded
but never written. So a pixel's level does not depend on which neighbours are
covered, which keeps tile and band splits, worker counts and triangle seams
invisible. The binned path does this in `quad_flush` after each triangle
(a bit per pixel in the tile's `qmask`); the immediate path has the same loop
in `raster_triangle`. `TEX` takes the differences inside the quad (fine
derivatives: `u[lane | 1] - u[lane & ~1]`, `u[(lane & 1) + 2] - u[lane & 1]`),
`lod = 0.5 * log2(max(|d/dx|^2, |d/dy|^2))` with the derivative of each
coordinate scaled by the level-0 size. The JIT calls the same C code
(`fgpu_texture_sample_batch`) for such samples and learns about quads through
bit 0 of its `textures` argument, so both agree bit for bit; the vertex stage
and single-lane runs sample level 0. `fgpu_jit_tests` runs every third batch
as quads on textures with chains, bias and clamps; `fgpu_mip_tests` checks
layout and box filter, level selection and blending against hand computed
references, quads drawn at known scales (nearest and trilinear, diagonal
seams, 1 and 3 workers, interpreter and JIT), a receding plane (the level
grows toward the horizon) and 16 texture units.

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

**Helpers.** The parallel phases run on `smp_par` (`smp_par.h`): the calling core
plus one preemptible kernel process `smp-par` per other core (not the master core),
woken for each job. A busy core's helper starts at its next tick and the scheduler
shares the core with programs; parts nobody took are done by the caller. The boot
flag `smpparipi` brings back the older helpers, which run inside an IPI handler with
interrupts off until the job ends.

**One pass for everyone.** The kernel keeps a single pass, used under the render
lock (see "Objects and contexts" above). The pass itself keeps its scratch in the
pass, but the batch buffers in `fgpu_raster.c` are still static per worker, so two
passes could not render at once yet.

### Present: scaled, paced, no back buffer (OS-5)

`SYS_GPU_PRESENT` to the screen (the screen owner only) used to copy the
surface into the kernel back buffer (`graphics_backbuffer_blit`) and then
copy the back buffer into the framebuffer (`graphics_backbuffer_present`):
two full-frame copies, the second one with plain stores into write-combining
memory. Now it is one: `graphics_present_scaled()` (`graphics.c`) calls
`gfx_scale_blit()` (`gfx_blit.c`), which expands each source row into a
960-pixel stack chunk (SSE2 shuffles, no `<emmintrin.h>`: the freestanding
build has no hosted headers) and writes the chunk to every destination row it
covers with 16-byte non-temporal stores (`movntdq`) and an `sfence` at the end.
Scale 1, 2 and 3 are nearest neighbour; the image is placed at `x, y` and
clipped on all four sides. The work is done in bands of 16 source rows with
the graphics lock (interrupts off) dropped between bands. No damage rectangle:
every present copies the whole surface.

The back buffer is only used by `SYS_GRAPHICS_BLIT` / `SYS_GRAPHICS_PRESENT`
(the terminal and its cursor draw straight into the framebuffer and stay off
while a program owns the screen), so it is kept lazily: a direct present marks
it stale, and the next back-buffer blit or rectangle present first reads the
framebuffer back into it. A program that mixes both kinds of present pays one
slow read of write-combining memory at each switch; a program that only uses
`SYS_GPU_PRESENT` never does. Screens that are not 32-bit XRGB with the usual
channel order scale into the back buffer and use the old converting present.
Page flipping (`FGPU_PRESENT_FLIP`) is unchanged and excludes scaling.

New flags (new bits of `FGPU_PRESENT_*`, everything else is rejected with
`SYS_ERR_INVAL`): `SCALE2`, `SCALE3` (screen only; exclusive with each other
and with `FLIP`) and `PACED`. Caps: `FGPU_CAP_SCALED_PRESENT` (1<<8),
`FGPU_CAP_PACED_PRESENT` (1<<9). The ABI version is not changed by this.

`PACED` waits for the next frame boundary of the pace interval (default
1/60 s; `SYS_GPU_SET_PACE`, op 16, `bytes` = interval in ns, 0 = default,
1 ms to 1 s) on `clock_monotonic_ns()` before presenting (screen or window).
Boundaries are `last + n * interval`; a present that is late by less than one
interval goes straight through and keeps the phase, later than that restarts
the phase. The wait is `sleep()` in whole milliseconds minus one, then a spin
of under 2 ms with interrupts on; the adapters here have no vblank to wait for.
If `out` is non-NULL, the present time (ns, taken just after the wait, before the copy) is written
there. Each program has its own pacer (in its GPU context), and the wait happens
before the render lock is taken, so other programs' GPU calls do not wait for it.

Benchmark (`make bench-gfx-scale`, host memory, not write-combining, so only a
CPU-side indication): 960x540 -> 1920x1080 2x about 0.6 ms standard stores,
about 0.7 ms with non-temporal stores. On the plan's budget (C4.7, 1.5 ms)
the real figure is set by write-combining throughput to the framebuffer, which
only a card can show. `tests/host/gfx_blit_tests.c` checks the scaler against
a pixel-by-pixel reference (1x/2x/3x, odd sizes, strides, offsets, clipping on
every side, both store kinds); `os0test` checks the flag combinations, the
timestamp pointer and the pace interval; `SCALE_CHECK=2 FGLCUBE_ARGS="--scale 2"
ONLY=new bash tools/debug/ci_fglcube.sh` checks a screenshot is made of 2x2 blocks.

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

### CPU JIT (OS-4)

`fgpu_jit_x64.c` compiles each shader stage to x86-64 machine code, with an
SSE4.1 path (4 lanes per XMM register) and an AVX2 path (8 lanes per YMM).
The interpreter stays the reference and the fallback: a program whose stage
does not compile, a CPU with only SSE2 (QEMU's default `qemu64` has no
SSE4.1) and the `nojit` boot flag all keep running on it. Nothing about the
call interface changed: `fgpu_shader_run_batch`, `run_vertex` and
`run_fragment` use the compiled code when the program has any.

**Pipeline.** bytecode -> `fgpu_ir_lower_ex(..., FGPU_IR_EXACT)` -> linear
scan register allocation (`fgpu_ir_allocate`) -> emitter -> code. The IR is
the branch-free SSA form the NVIDIA backend uses (jumps are if-converted to
selects, so the code has no branches at all). `FGPU_IR_EXACT` makes the IR
mean what the interpreter computes instead of what GPU hardware would:
IEEE division instead of multiply-by-reciprocal, a real subtraction, no
algebraic simplifications that change `-0`/NaN results (`x*1`, `x+0`,
`fma(a,0,c)`, `min(a,a)`), no commuting of `min`/`max`, `step` as
`!(b < a)`, `abs` as the interpreter's `x < 0 ? -x : x` (which keeps `-0`),
and a pending negation is materialized before an `abs` reads it. The
transcendental ops (`EX2`, `LG2`, `SIN`, `COS`, `RSQ`) are emitted as the
interpreter's own polynomial sequences, one vector instruction per vector
operation, with the same constants; `fma` is multiply, round, add (no FMA
instruction, so results are bit-identical; the code uses AVX2 but not FMA).

**Results are bit-identical** to the interpreter for every opcode,
including `-0`, denormals, infinities, float-to-int conversions, `min`/`max`
operand order and comparisons with NaN. The one allowed difference: the
sign/payload of a NaN that comes out of arithmetic on two NaNs (operand
order, which FGPU does not define) -- a NaN is compared as a NaN. Also,
discarded lanes keep unspecified outputs (the interpreter freezes them at
the `discard`, the JIT runs on), and temporaries are not preserved (the
interpreter leaves its last values in `batch->temp`; the JIT uses that area
as its spill space). Lanes not asked for (`lanes` mask) get the default
outputs in both.

**Registers.** `xmm0` is the blend mask (SSE `blendvps`), `xmm1`..`xmm8`
hold IR values (virtual registers 0..7 of the allocator; the allocator
hands out the lowest free number, so the hot values land there), the other
virtual registers are spill slots, `xmm9`..`xmm15` are scratch. Spill slots,
the texture helper's arguments and results, and the registers saved around
a helper call all live in the batch's `temp` rows (128 slots of 32 bytes;
a stage that needs more does not compile). General registers: `rbx` = the
batch (`+16` bytes for the SSE upper half), `rbp` = uniforms, `r12` =
textures, saved and restored; `rax, rcx, rdx, rsi, rdi, r8, r9` only for
helper calls.

**One emitter, two ISAs.** A small layer (`v3`, `v2`, `cmp`, `shift`,
`sel`) takes AVX-style three-operand requests. For AVX2 it emits 3-byte
VEX with L=1; for SSE4.1 it emits the legacy two-operand form, inserting a
`movaps` when the destination is not the first source, and `blendvps` with
the mask in `xmm0` for selects. The IR code generation is shared. The SSE
path runs the code twice, once per half of the 8-lane batch (base pointer
+16 bytes); empty halves are skipped. The AVX2 path ends with
`vzeroupper` (and has one before every helper call) so SSE code
afterwards pays no transition penalty.

**Calling convention.** `uint32_t code(char* batch, const float* uniforms,
const fgpu_texture_t* textures)` (System V): returns the lanes that were
not discarded (4 or 8 bits). The wrapper masks that with the `lanes`
argument, and writes the default outputs for lanes that were not asked for.

**Helper-call ABI.** The only call is texture sampling: `jit_tex_helper`
(C, in the same file), at a fixed address, called with `rdi` = textures,
`esi` = unit, `rdx`/`rcx` = rows of u and v, `r8` = result rows, `r9d` =
lanes. It calls the interpreter's own `fgpu_texture_sample` per lane, so
the results are identical. Before the call the code stores every live
value that is in an XMM register to its save slot and reloads it
afterwards (all vector registers are caller-saved in System V); results
stay in slots, 4 per sample, until the last use.

**Safety (the JIT output runs in ring 0).**

- It is generated only from IR that was lowered from a program that passed
  `fgpu_program_check`; the emitter checks again that every register file,
  input/output/special/uniform slot and texture unit index is in range and
  fails the compile otherwise.
- Memory the code can touch, all at offsets fixed when it is generated:
  the batch's input, special, output and temp rows (inputs are only read),
  the uniforms (read only), and its own constant pool (read only, 32-byte
  aligned). Nothing is computed from shader data: there are no
  indirect jumps or calls, no jumps at all, no data-dependent addresses.
  The one call goes to a fixed helper that clamps every texel coordinate.
- The code buffer is a fixed-size scratch area; the emitter checks every
  byte and the compile fails (the stage stays on the interpreter) on
  overflow, too many constants, too many texture samples or spill slots,
  or more than 4096 IR operations. Compile time and code size are
  therefore bounded (at most 128 KiB of code per stage; real shaders are
  2-10 KiB). The finished code is copied to an exactly sized block.
- Compilation allocates, so it happens when the program is created
  (`op_create_program`, process context), never in a worker. The code is
  cached in `fgpu_program_t.jit` and freed with the program
  (`fgpu_program_release`, called from `program_free`).
- W^X: on the host the code is mapped RW, copied, then `mprotect`ed RX. In
  the kernel the code comes from the heap, which is in the low identity map
  and is writable and executable today; there is nothing to flip yet. What
  should change: allocate whole pages from the page pool, map them RW
  while writing, then RX (and keep the rest of the heap NX).
- Vector state: AVX code only exists as JIT output (the kernel is compiled
  without AVX). Every kernel entry stub (`simd_save.inc`: exceptions,
  IRQs, `int 0x80`, and the `smp_par` IPI wrapper in `idt_.asm`) saves the
  full XSAVE area, including the upper YMM halves, before C code runs and
  restores it on the way out, so JIT code running in a syscall or in a
  helper core's IPI handler cannot corrupt the interrupted program's
  state. `vzeroupper` before returning keeps the kernel's SSE code fast.
  Exceptions the JIT code could raise are masked in MXCSR like everywhere
  in the kernel; both machines run under the same MXCSR.
- CPU gating: SSE4.1 from CPUID; AVX2 only when `cpu_simd_features()` says
  the OS enabled the state (XSAVE, `noavx` flag not given). `nojit` forces
  the interpreter. All CPUs are assumed to match the boot CPU (as the
  entry stubs already do).

**Flags and API.** Boot flag `nojit` (GRUB entry "Minimal-OS (RamFS,
nojit)"); `fgpu_jit_set_mode(OFF|AUTO|SSE41|AVX2)` for tests and benchmarks;
`gpu jit` in the terminal runs the kernel self-check (below).

**Tests.** `tests/host/fgpu_jit_tests.c` (part of `make test-host`):
per-opcode tests over all pairs/triples of ~50 special operands with
modifiers, swizzles and write masks; control flow; texture units; real FGSL
shaders (including both `fglcube --shader` shaders); a deterministic
differential fuzzer over random valid programs; safety and negative tests.
`make fuzz-fgpu-jit FUZZ_SECONDS=300` runs the fuzzer longer. Kernel:
`gpu jit` compiles FGSL shaders in the kernel and compares interpreter, SSE4.1
and AVX2 code on random batches, also on the other cores through `smp_par`
(`tools/debug/ci_fgpu_jit.sh` drives it in QEMU). `tools/debug/ci_fgpu_jit_cube.sh`
takes screendumps of `fglcube --shader --still` to compare JIT and `nojit`.

**Speed** (`make bench-fgpu-jit`, one thread of a 2.1 GHz Xeon, through the
rasterizer's batch interface; lane-instructions are FGSL bytecode
instructions x 8 lanes):

```
shader (stage)                instr     interp     SSE4.1       AVX2    SSE x   AVX2 x
                                    G lane-i/s G lane-i/s G lane-i/s
arith vertex (~25 instr)         28      0.399      4.185      7.098    10.5x    17.8x
arith fragment                   15      0.508      2.360      3.842     4.6x     7.6x
fragment + bilinear tex           5      0.116      0.137      0.152     1.2x     1.3x
fglcube vertex                   20      0.454      3.119      5.615     6.9x    12.4x
fglcube fragment                 61      0.377      0.940      1.194     2.5x     3.2x
```

The plan's target (16 G lane-instr/s AVX2, 0.6x of it with SSE4.1) is for a
4 GHz core on shaders that are mostly arithmetic: the 28-instruction vertex
shader reaches 7.1 G (AVX2) and 4.2 G (SSE4.1) here, about 44% of the target on AVX2 (the SSE path is 59% of the AVX2 one); the fragment shaders with `pow`/`sin`/`log2`/texture are far from it.
The interpreter number is this host's (GCC -O3), not the kernel's 0.1-0.2.

Where the time goes: arithmetic is now limited by the number of vector
instructions (the transcendentals are 25-60 each), not by dispatch. Two
things cap the next factor, and both are interface/design limits:
(1) texture sampling runs through a scalar C helper per lane (about 30 ns
per bilinear sample), so a shader with one sample is dominated by it; an
inline AVX2 gather sampler for the common ARGB8 path would remove that;
(2) marshalling: the rasterizer scatters its per-pixel data into the
8-lane SoA rows and gathers the outputs back, which costs about twice the
shader for a short fragment shader (see the benchmark line). A call that
takes the interpolants and writes pixels itself (fused kernels, as in the
plan) would remove it, as would larger batches (16/32 lanes per call, so
the 4-6 ns call and per-batch constant loads amortize).

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
   draws in two stages: first the near triangle alone, without depth,
   into a pitch-linear 64x64 surface; then both triangles with depth test
   and back-face culling into a **block-linear** target, which the copy
   engine copies back to the surface before three pixels are checked:

   ```text
   Step 4b: FarcadeGL draws on the 3D engine
     shaders: vertex and pixel stage compiled (19 + 13 registers)
     without depth: centre 0xff4080bf, corner 0xff102030 (ok)
     clockwise front: centre 0xff4080bf, left 0xffff0000, corner 0xff102030
     ok: triangles, shaders, depth test and culling work on the 3D engine.
   ```

   First run on an RTX 2060 (2026-11-17): the clears of step 4a worked,
   but the first job of step 4b failed with `GSP-RM: channel error (RC)
   on engine 0x1, channel 8, exception 69` (69 =
   `ROBUST_CHANNEL_GR_CLASS_ERROR`, Xid 69: the graphics engine refused a
   method or its data). That job cleared and drew into a **pitch-linear**
   color target with a depth buffer bound, which the hardware does not
   allow (nouveau's data error name: `RT_LINEAR_WITH_ZETA`; NVK renders
   linear images through a tiled shadow for the same reason). Changes:
   - draws with depth (and clears of color with depth) go to a
     block-linear copy of the color surface that the backend keeps
     (`tiled` in `fgpu_backend_nv.c`); the copy engine brings it back to
     the FGPU surface (`nv_rm_ce_copy_surface`, block-linear to pitch)
     in `finish()`, before a read, before a draw samples that surface,
     and before a draw falls back to the CPU; a depth-only clear binds no
     color target;
   - vertex attributes that come from no stream now carry a valid format
     (R32 float, as nouveau's nvc0 does) instead of 0, which is no value
     of `COMPONENT_BIT_WIDTHS` or `NUMERICAL_TYPE`;
   - when a 3D job fails, the log shows what PGRAPH trapped:
     `PGRAPH trap: status ..., subchannel S method 0xMMMM data 0x...,
     error 0xE NAME` (from 0x400100/0x400704/0x400708/0x400110, as
     nouveau's gf100_gr_intr reads them), which names the method.

   Second run (2026-11-18): NVDEC came up (`ok: the video decoder's
   channel runs`) and the 3D clear worked, but the draw failed with
   `Graphics Exception: Shader Program Header 11 Error`, ESR
   `0x405840 = 0xa0000800` (`NV_PGRAPH_PRI_DS_HWW_ESR`: SPH11, pipe stage
   0x20 = pixel) and RC exception 13. The pixel shader header lacked
   `IMAP_POSITION_W` (bit 191); the card wants it in every pixel header,
   as Mesa's nvc0 driver notes ("getting a trap if FRAG_COORD_UMASK.w =
   0"). `nv_sass.c` now sets it, `nv_sass_tests` checks it, and a failed
   3D job also prints `DS trap: shader header errors 0x... in the ...
   stage` from that register.

   Third run (2026-11-18): no header error any more, but every SM
   reported `Graphics SM Warp Exception: Out Of Range Register` (warp ESR
   `0x...730 = 0x..0d`, PC `0x174`) and RC exception 13. Since Volta each
   thread's program counter lives in two registers after the ones the
   code uses (NAK's `hw_reserved_gprs`), so `SET_PIPELINE_REGISTER_COUNT`
   has to count them; we gave the bare count. `nv_sass_encode` now adds
   `NV_SASS_HW_RESERVED_REGS` (2) to `regs`, and `nv_sass_tests` checks
   that the code stays below `regs - 2`.

   Fourth run (2026-11-18): the 3D engine draws, but the Farcade editor
   took 800 ms a frame on Medium and 80 ms on Low. Medium's lit shader
   reads its lights and shadow data from a storage buffer, and
   `nv_sass.c` refused every storage buffer load, so every lit draw ran
   on the CPU between GPU passes, and each one copied the render target
   out of its block-linear copy and back. Mipmapped textures did the
   same. Now:
   - storage buffer `b` is bound as constant buffer `1 + b` (up to
     64 KiB, the constant cache dropped on each draw), and loads with a
     constant index, which is every load in the engine's shaders, are
     `c[1+b][16 * index + 4 * comp]` operands. A computed index still
     stays on the CPU (it would need `LDC` with a register offset);
   - mipmapped textures are drawn on the card from level 0 (pitch-linear
     textures cannot have mips);
   - a job no longer polls the GSP-RM queue before it has waited 5 ms;
   - `gpu` reports the jobs and their average time, the time spent in
     block-linear copies, the draws offered to the card and the ones the
     CPU did with the reason. Run it twice around a few seconds in the
     editor to see where a slow frame goes.

   The centre is inside both triangles (the near, blue-ish one must win),
   `left` only inside the far red one, `corner` in neither. If the first
   try shows only the clear color, the test repeats with the other
   winding as "front" and keeps what works. What other results mean:

   | Result | Look at |
   | --- | --- |
   | `the draw without depth did not finish` or `the test draw did not finish`, with `channel error (RC)` or `GPU page fault` above it | a method or address the engine rejects: `nv3d_push_init` / `nv3d_push_draw` in `nv_3d.c`; the `PGRAPH trap` line names the method offset and the error |
   | all three pixels are the clear color with both windings | nothing was rasterized: vertex stream/attribute setup, the vertex stage's code or header (`nv_sass.c`), or the viewport |
   | centre and left have a color, but the wrong one | the pixel stage: interpolation (`IPA`), the output registers, or the constant buffer |
   | centre is red | the depth buffer: block-linear layout or depth state |

   After `ok`, FGPU puts new color surfaces, textures and buffers in VRAM
   and sends clears and draws to the 3D engine. `gpu nvidia` shows how
   many ran. A job that does not finish later switches the 3D path off
   and rendering continues on the CPU.

5. **Step 5a** (video decoder): a channel on NVDEC with the decoder
   class (`NVC4B0_VIDEO_DECODER`); GSP-RM runs NVDEC's firmware itself,
   but like every falcon engine NVDEC needs an engine context buffer from
   the driver, promoted to the channel before the class is allocated
   (nouveau `fifo/r535.c`: `r535_fifo_ectx_size`, `r535_flcn_ctor`). The
   size comes from RM's internal `GET_CONSTRUCTED_FALCON_INFO` for NVDEC's
   engine descriptor (`engine context: N KiB`). Without it the first run
   on an RTX 2060 got `video decoder object (class 0xC4B0): RM error 0x1F`
   (`NV_ERR_INVALID_ARGUMENT`). Then it prints `channel N on NVDEC
   (runlist R)` and `ok: the video decoder's channel runs.` after an
   empty first job. `gpu nvidia` then
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
