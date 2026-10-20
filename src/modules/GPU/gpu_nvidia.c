#include "x86_64/gpucore.h"
#include "serial.h"
#include "string.h"

/*
 * NVIDIA GeForce: GTX 10 series (Pascal, GP10x) and RTX 20 / GTX 16
 * series (Turing, TU10x/TU11x). Other generations are recognised and get
 * the same basic support.
 *
 * What this driver does:
 *  - identifies the chip from PMC_BOOT_0 and the PCI device id,
 *  - reads the amount of video memory from the memory controller,
 *  - maps BAR1 (the CPU's window onto VRAM) write-combining,
 *  - finds the framebuffer the firmware (UEFI GOP / VBIOS) set up inside
 *    VRAM and keeps drawing there,
 *  - hands the rest of the window out as VRAM for buffers,
 *  - reads the core temperature.
 *
 * What it does not do (yet): change display modes, page flip, or run work
 * on the GPU's own engines. Since Maxwell 2 the graphics engine only runs
 * NVIDIA-signed firmware, which has to be loaded through the secure boot
 * falcon; that is a large project of its own (nouveau's "ACR"). Rendering
 * therefore uses the FGPU CPU backend, drawing into VRAM.
 *
 * Register offsets are from nouveau (drm/nouveau/nvkm).
 */

#define NV_PMC_BOOT_0        0x000000
#define NV_PBUS_BAR1_BLOCK   0x001704   // Fermi-Volta, bit 31: BAR1 uses a GPU page table
#define NV_BAR1_BLOCK_TU     0xB80F48   // Turing and later, same meaning
#define NV_PFB_MEM_SIZE_GP   0x100CE0   // Pascal (GP102+), Volta, Turing: size encoding
#define NV_PFB_MEM_SIZE_GA   0x1183A4   // Ampere (GA102) and later: size in MiB
#define NV_THERM_TSENSOR_GP  0x020460

typedef struct {
    uint16_t device_id;
    const char* name;
} nv_name_t;

static const nv_name_t g_names[] = {
    // Pascal (GTX 10)
    { 0x1B00, "TITAN X (Pascal)" },     { 0x1B06, "GeForce GTX 1080 Ti" },
    { 0x1B80, "GeForce GTX 1080" },     { 0x1B81, "GeForce GTX 1070" },
    { 0x1B82, "GeForce GTX 1070 Ti" },  { 0x1B83, "GeForce GTX 1060 6GB" },
    { 0x1B84, "GeForce GTX 1060 3GB" }, { 0x1C02, "GeForce GTX 1060 3GB" },
    { 0x1C03, "GeForce GTX 1060 6GB" }, { 0x1C04, "GeForce GTX 1060 5GB" },
    { 0x1C06, "GeForce GTX 1060 6GB" }, { 0x1C81, "GeForce GTX 1050" },
    { 0x1C82, "GeForce GTX 1050 Ti" },  { 0x1C83, "GeForce GTX 1050 3GB" },
    { 0x1D01, "GeForce GT 1030" },      { 0x1D02, "GeForce GT 1010" },
    // Turing (RTX 20 / GTX 16)
    { 0x1E02, "TITAN RTX" },            { 0x1E04, "GeForce RTX 2080 Ti" },
    { 0x1E07, "GeForce RTX 2080 Ti" },  { 0x1E81, "GeForce RTX 2080 SUPER" },
    { 0x1E82, "GeForce RTX 2080" },     { 0x1E84, "GeForce RTX 2070 SUPER" },
    { 0x1E87, "GeForce RTX 2080" },     { 0x1F02, "GeForce RTX 2070" },
    { 0x1F03, "GeForce RTX 2060 12GB" },{ 0x1F06, "GeForce RTX 2060 SUPER" },
    { 0x1F07, "GeForce RTX 2070" },     { 0x1F08, "GeForce RTX 2060" },
    { 0x1F42, "GeForce RTX 2060 SUPER" },{ 0x1F47, "GeForce RTX 2060 SUPER" },
    { 0x1F11, "GeForce RTX 2060 (laptop)" }, { 0x1F15, "GeForce RTX 2060 (laptop)" },
    { 0x1F82, "GeForce GTX 1650" },     { 0x2182, "GeForce GTX 1660 Ti" },
    { 0x2184, "GeForce GTX 1660" },     { 0x2187, "GeForce GTX 1650 SUPER" },
    { 0x21C4, "GeForce GTX 1660 SUPER" },
};

typedef struct {
    uint16_t chipset;
    const char* family;
} nv_chip_t;

static const nv_chip_t g_chips[] = {
    { 0x117, "Maxwell (GM107)" }, { 0x118, "Maxwell (GM108)" },
    { 0x120, "Maxwell (GM200)" }, { 0x124, "Maxwell (GM204)" },
    { 0x126, "Maxwell (GM206)" },
    { 0x130, "Pascal (GP100)" },  { 0x132, "Pascal (GP102)" },
    { 0x134, "Pascal (GP104)" },  { 0x136, "Pascal (GP106)" },
    { 0x137, "Pascal (GP107)" },  { 0x138, "Pascal (GP108)" },
    { 0x140, "Volta (GV100)" },
    { 0x162, "Turing (TU102)" },  { 0x164, "Turing (TU104)" },
    { 0x166, "Turing (TU106)" },  { 0x167, "Turing (TU117)" },
    { 0x168, "Turing (TU116)" },
    { 0x172, "Ampere (GA102)" },  { 0x173, "Ampere (GA103)" },
    { 0x174, "Ampere (GA104)" },  { 0x176, "Ampere (GA106)" },
    { 0x177, "Ampere (GA107)" },
    { 0x192, "Ada (AD102)" },     { 0x193, "Ada (AD103)" },
    { 0x194, "Ada (AD104)" },     { 0x196, "Ada (AD106)" },
    { 0x197, "Ada (AD107)" },
};

static uint32_t nv_rd32(gpu_adapter_t* a, uint32_t reg) {
    if (!a->mmio || reg + 4 > a->mmio_size) return 0xFFFFFFFFu;
    return a->mmio[reg / 4];
}

static bool nv_is_turing_or_newer(uint16_t chipset) {
    return chipset >= 0x160;
}

static bool nv_is_ampere_or_newer(uint16_t chipset) {
    return chipset >= 0x170;
}

static uint64_t nv_vram_size(gpu_adapter_t* a, uint16_t chipset) {
    uint64_t size = 0;
    // nouveau: gp102_fb_vidmem_size (Pascal..Turing), ga102_fb_vidmem_size.
    // Maxwell reports sizes per memory partition; left to the BAR1 size.
    if (nv_is_ampere_or_newer(chipset)) {
        size = (uint64_t)nv_rd32(a, NV_PFB_MEM_SIZE_GA) << 20;
    } else if (chipset >= 0x130) {
        uint32_t data = nv_rd32(a, NV_PFB_MEM_SIZE_GP);
        uint32_t lmag = (data & 0x3F0) >> 4;
        uint32_t lsca = data & 0xF;
        size = (uint64_t)lmag << (lsca + 20);
        if (data & 0x40000000) size = size / 16 * 15;   // ECC carve-out
    }
    // Plausible boards have 256 MiB to 96 GiB.
    if (size < (256ull << 20) || size > (96ull << 30)) size = 0;
    return size;
}

static int nv_temperature(gpu_adapter_t* a, uint16_t chipset) {
    if (chipset < 0x130) return -1;
    uint32_t t = nv_rd32(a, NV_THERM_TSENSOR_GP);
    if (t == 0xFFFFFFFFu || !(t & 0x20000000)) return -1;
    int celsius = (int)((t & 0x0001FFF8) >> 8);
    return (celsius > 0 && celsius < 150) ? celsius : -1;
}

static bool nvidia_probe(gpu_adapter_t* a, pci_device_t* pci) {
    if (pci->vendor_id != 0x10DE || pci->class_code != 0x03) return false;
    a->vendor = GPU_VENDOR_NVIDIA;

    // BAR0: registers. Only the first 16 MiB exist on every generation.
    if (pci->bar_type[0] == PCI_BAR_MEM && pci->bar_length[0]) {
        a->mmio_size = pci->bar_length[0] < 0x1000000 ? pci->bar_length[0] : 0x1000000;
        a->mmio = (volatile uint32_t*)gpu_map_bar(pci, 0, a->mmio_size, false);
    }

    uint32_t boot0 = nv_rd32(a, NV_PMC_BOOT_0);
    uint16_t chipset = (uint16_t)((boot0 >> 20) & 0x1FF);
    if (boot0 == 0xFFFFFFFFu) chipset = 0;

    const char* name = NULL;
    for (uint32_t i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        if (g_names[i].device_id == pci->device_id) { name = g_names[i].name; break; }
    }
    if (name) snprintf(a->name, sizeof(a->name), "NVIDIA %s", name);
    else snprintf(a->name, sizeof(a->name), "NVIDIA GPU (device 0x%x)", (unsigned)pci->device_id);

    const char* family = NULL;
    for (uint32_t i = 0; i < sizeof(g_chips) / sizeof(g_chips[0]); i++) {
        if (g_chips[i].chipset == chipset) { family = g_chips[i].family; break; }
    }
    if (family) strncpy(a->family, family, sizeof(a->family) - 1);
    else snprintf(a->family, sizeof(a->family), "chipset 0x%x", (unsigned)chipset);

    a->vram_size = nv_vram_size(a, chipset);

    // BAR1: the VRAM window (64-bit prefetchable, usually 256 MiB, all of
    // VRAM with Resizable BAR).
    int bar1 = 1;
    if (pci->bar_type[bar1] == PCI_BAR_MEM && pci->bar_length[bar1]) {
        a->aperture_phys = pci->bar_address[bar1];
        a->aperture_size = pci->bar_length[bar1];
        if (a->vram_size && a->aperture_size > a->vram_size) a->aperture_size = a->vram_size;
        a->aperture = (volatile uint8_t*)gpu_map_bar(pci, bar1, a->aperture_size, true);
    }
    if (!a->vram_size) a->vram_size = a->aperture_size;

    // BAR1 offsets equal VRAM offsets only while BAR1 is in physical mode
    // (as the firmware leaves it). If a BAR1 page table is active (bit 31
    // of the BAR1 block register), keep out of VRAM.
    uint32_t bar1_block = nv_rd32(a, nv_is_turing_or_newer(chipset) ? NV_BAR1_BLOCK_TU
                                                                    : NV_PBUS_BAR1_BLOCK);
    bool bar1_virtual = bar1_block != 0xFFFFFFFFu && (bar1_block & 0x80000000u);
    if (a->aperture && !bar1_virtual) a->caps |= GPU_CAP_VRAM_HEAP;

    int temp = nv_temperature(a, chipset);
    if (temp >= 0) {
        snprintf(a->notes, sizeof(a->notes),
                 "%d C; display mode from firmware; 3D runs on the FGPU CPU backend", temp);
    } else {
        strcpy(a->notes, "display mode from firmware; 3D runs on the FGPU CPU backend");
    }
    serial_write_str("nvidia: PMC_BOOT_0=0x");
    serial_write_hex(boot0);
    serial_write_str("\n");
    return true;
}

const gpu_adapter_driver_t gpu_nvidia_adapter_driver = {
    .probe = nvidia_probe,
    .name = "nvidia",
};
