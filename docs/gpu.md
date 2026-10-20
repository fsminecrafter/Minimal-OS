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

The display mode is the one the firmware set (GOP/VBE). Running work on
the GPU's own engines needs NVIDIA-signed firmware loaded through the
secure-boot falcon on these generations; that is not implemented, so 3D
renders on the CPU (below) into VRAM.

### QEMU / Bochs

`gpu_bochs.c` reads the VRAM size and can page flip (show any line of
VRAM). Because VRAM is ordinary host memory there, the kernel's screen
back buffer is moved into VRAM as well. Give QEMU more VRAM with
`-vga none -device VGA,vgamem_mb=64` (`make run` does).

## FGPU command processor

`SYS_GPU` (40) is the GPU interface for programs (`fgpu_abi.h`, shared
with the SDK). A process creates *surfaces* (color XRGB8888, depth float32)
and *buffers* (vertices, indices), records a command buffer (bind target,
viewport, clear, state, program, uniforms, draw) and submits it; then
presents a color surface to the screen (copy, or page flip when the
surface is in VRAM and the adapter can flip) or into a window.
Everything is freed when the process exits.

The CPU backend (`fgpu_raster.c`) runs the pipeline: vertex transform and
lighting, near-plane clipping, perspective-correct rasterization with the
top-left rule, depth test, back/front face culling, wireframe, and three
built-in programs (vertex color, Gouraud, per-pixel Phong with
Blinn-Phong highlights). `make test-host` runs its tests. Programs are
fixed today; the command format has room for shader code.

## FarcadeGL

The SDK's `farcadegl.h` wraps all of this in an OpenGL/Vulkan-like API
(see `Minimal-OS-SDK/docs/farcadegl.md`). `fglcube` (terminal, full
screen) and MOSdesk's *Cube* app are examples.
