#include "x86_64/gpucore.h"
#include "x86_64/port.h"
#include "serial.h"
#include "string.h"

/*
 * QEMU / Bochs standard VGA (PCI 1234:1111), also QEMU's bochs-display.
 *
 * BAR0 is the whole video memory. The "DISPI" registers (I/O ports
 * 0x1CE/0x1CF) report its size and set the mode; the virtual height and
 * Y offset registers let the display show any line of VRAM, which is how
 * page flipping works on this device.
 */

#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF
#define VBE_ID           0x0
#define VBE_XRES         0x1
#define VBE_YRES         0x2
#define VBE_BPP          0x3
#define VBE_VIRT_WIDTH   0x6
#define VBE_VIRT_HEIGHT  0x7
#define VBE_X_OFFSET     0x8
#define VBE_Y_OFFSET     0x9
#define VBE_VIDEO_MEM_64K 0xA

static uint16_t vbe_read(uint16_t index) {
    port_outw(VBE_INDEX, index);
    return port_inw(VBE_DATA);
}

static void vbe_write(uint16_t index, uint16_t value) {
    port_outw(VBE_INDEX, index);
    port_outw(VBE_DATA, value);
}

static bool bochs_flip(gpu_adapter_t* a, uint64_t offset) {
    uint16_t xres = vbe_read(VBE_XRES);
    uint16_t bpp = vbe_read(VBE_BPP);
    if (!xres || bpp != 32) return false;
    uint64_t pitch = (uint64_t)xres * 4;
    if (offset % pitch) return false;
    uint64_t lines = a->aperture_size / pitch;
    if (lines > 0xFFFF) lines = 0xFFFF;
    if (offset / pitch + vbe_read(VBE_YRES) > lines) return false;
    vbe_write(VBE_VIRT_WIDTH, xres);
    vbe_write(VBE_VIRT_HEIGHT, (uint16_t)lines);
    vbe_write(VBE_X_OFFSET, 0);
    vbe_write(VBE_Y_OFFSET, (uint16_t)(offset / pitch));
    return true;
}

static bool bochs_probe(gpu_adapter_t* a, pci_device_t* pci) {
    if (pci->vendor_id != 0x1234 || pci->device_id != 0x1111) return false;
    uint16_t id = vbe_read(VBE_ID);
    if (id < 0xB0C0 || id > 0xB0CF) return false;

    a->vendor = GPU_VENDOR_QEMU;
    strcpy(a->name, "QEMU/Bochs standard VGA");
    snprintf(a->family, sizeof(a->family), "Bochs DISPI 0x%x", (unsigned)id);
    uint64_t vram = (uint64_t)vbe_read(VBE_VIDEO_MEM_64K) * 64 * 1024;
    a->aperture_phys = pci->bar_address[0];
    a->aperture_size = pci->bar_length[0];
    a->vram_size = vram ? vram : a->aperture_size;
    if (a->vram_size < a->aperture_size) a->aperture_size = a->vram_size;
    a->aperture = (volatile uint8_t*)gpu_map_bar(pci, 0, a->aperture_size, true);
    a->caps = GPU_CAP_MODESET | GPU_CAP_VRAM_FAST_READ;
    if (a->aperture) {
        a->caps |= GPU_CAP_VRAM_HEAP | GPU_CAP_PAGE_FLIP;
        a->flip = bochs_flip;
    }
    strcpy(a->notes, "virtual GPU: VRAM is host memory; 3D runs on the FGPU CPU backend");
    return true;
}

const gpu_adapter_driver_t gpu_bochs_adapter_driver = {
    .probe = bochs_probe,
    .name = "bochs",
};
