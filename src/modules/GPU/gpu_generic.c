#include "x86_64/gpucore.h"
#include "x86_64/gpu.h"
#include "prochandler.h"
#include "string.h"

/*
 * Any other display controller (AMD, Intel, VMware, virtio, ...).
 *
 * Without a chip-specific driver the only known piece of video memory is
 * the PCI memory BAR that holds the firmware's framebuffer. That BAR is
 * used as the VRAM window; space after the framebuffer is used only if it
 * reads back what was written (gpucore.c checks).
 */

static const char* vendor_name(uint16_t vendor, gpu_vendor_t* out) {
    switch (vendor) {
        case 0x1002: *out = GPU_VENDOR_AMD; return "AMD";
        case 0x8086: *out = GPU_VENDOR_INTEL; return "Intel";
        case 0x15AD: *out = GPU_VENDOR_VMWARE; return "VMware";
        case 0x1AF4: *out = GPU_VENDOR_VIRTIO; return "virtio";
        case 0x10DE: *out = GPU_VENDOR_NVIDIA; return "NVIDIA";
        case 0x1234: *out = GPU_VENDOR_QEMU; return "QEMU";
        default: *out = GPU_VENDOR_OTHER; return "Unknown vendor";
    }
}

static bool generic_probe(gpu_adapter_t* a, pci_device_t* pci) {
    const char* vendor = vendor_name(pci->vendor_id, &a->vendor);
    snprintf(a->name, sizeof(a->name), "%s display controller 0x%x", vendor,
             (unsigned)pci->device_id);
    strcpy(a->family, "generic");

    // The BAR holding the current framebuffer, else the biggest memory BAR.
    int bar = -1;
    gpu_device_t* screen = getSystemGPU();
    if (screen && screen->fb_phys) {
        bar = pci_bar_containing(pci, screen->fb_phys,
                                 (uint64_t)screen->pitch * screen->height);
    }
    if (bar < 0) {
        uint64_t best = 0;
        for (int i = 0; i < PCI_NUM_BARS; i++) {
            if (pci->bar_type[i] == PCI_BAR_MEM && pci->bar_length[i] > best) {
                best = pci->bar_length[i];
                bar = i;
            }
        }
    }
    if (bar >= 0) {
        a->aperture_phys = pci->bar_address[bar];
        a->aperture_size = pci->bar_length[bar];
        a->aperture = (volatile uint8_t*)gpu_map_bar(pci, bar, a->aperture_size, true);
        a->vram_size = a->aperture_size;    // at least this much
        if (a->aperture) a->caps |= GPU_CAP_VRAM_HEAP;
    }
    strcpy(a->notes, "no chip driver: framebuffer only; 3D runs on the FGPU CPU backend");
    return true;
}

const gpu_adapter_driver_t gpu_generic_adapter_driver = {
    .probe = generic_probe,
    .name = "generic",
};
