#include "x86_64/gpucore.h"
#include "x86_64/fgpu.h"
#include "x86_64/gpu.h"
#include "x86_64/mmio.h"
#include "x86_64/spinlock.h"
#include "prochandler.h"
#include "graphics.h"
#include "serial.h"
#include "string.h"

typedef struct {
    bool used;
    uint64_t offset;
    uint64_t size;
    uint64_t owner;
    uint32_t adapter;
} vram_block_t;

static gpu_adapter_t g_adapters[GPU_MAX_ADAPTERS];
static uint32_t g_adapter_count;
static int g_primary = -1;
static vram_block_t g_blocks[GPU_VRAM_MAX_BLOCKS];
static spinlock_t g_vram_lock = SPINLOCK_INIT;

static const gpu_adapter_driver_t* const g_drivers[] = {
    &gpu_nvidia_adapter_driver,
    &gpu_bochs_adapter_driver,
    &gpu_generic_adapter_driver,     // last: takes anything
};

volatile void* gpu_map_bar(pci_device_t* pci, int bar, uint64_t length, bool vram) {
    if (!pci || bar < 0 || bar >= PCI_NUM_BARS || pci->bar_type[bar] != PCI_BAR_MEM) {
        return NULL;
    }
    uint64_t phys = pci->bar_address[bar];
    if (!length || length > pci->bar_length[bar]) length = pci->bar_length[bar];
    if (!phys || !length) return NULL;
    length = (length + 0xFFF) & ~0xFFFull;
    void* va = mmio_alloc_va(length);
    if (!va) return NULL;
    bool ok = vram ? map_mmio_page_wc((uintptr_t)va, phys, length)
                   : map_mmio_page((uintptr_t)va, phys, length);
    return ok ? (volatile void*)va : NULL;
}

uint32_t gpu_adapter_count(void) {
    return g_adapter_count;
}

gpu_adapter_t* gpu_adapter_get(uint32_t index) {
    return index < g_adapter_count ? &g_adapters[index] : NULL;
}

gpu_adapter_t* gpu_primary_adapter(void) {
    if (g_primary >= 0) return &g_adapters[g_primary];
    return g_adapter_count ? &g_adapters[0] : NULL;
}

/* ---------------------------------------------------------------- VRAM */

uint64_t gpu_vram_alloc(gpu_adapter_t* adapter, uint64_t size, uint64_t align,
                        uint64_t owner_pid) {
    if (!adapter || !(adapter->caps & GPU_CAP_VRAM_HEAP) || !adapter->aperture ||
        size == 0) {
        return GPU_VRAM_INVALID;
    }
    if (align < 4096) align = 4096;
    size = (size + 4095) & ~4095ull;
    uint32_t index = (uint32_t)(adapter - g_adapters);

    uint64_t flags = spinlock_acquire(&g_vram_lock);
    // First fit: walk candidate start points (heap start and the end of
    // every block) and take the first that overlaps nothing.
    uint64_t best = GPU_VRAM_INVALID;
    for (int c = -1; c < GPU_VRAM_MAX_BLOCKS && best == GPU_VRAM_INVALID; c++) {
        uint64_t start;
        if (c < 0) start = adapter->heap_start;
        else if (g_blocks[c].used && g_blocks[c].adapter == index)
            start = g_blocks[c].offset + g_blocks[c].size;
        else continue;
        start = (start + align - 1) / align * align;
        if (start < adapter->heap_start || start + size > adapter->heap_end) continue;
        bool clash = false;
        for (int i = 0; i < GPU_VRAM_MAX_BLOCKS; i++) {
            vram_block_t* b = &g_blocks[i];
            if (!b->used || b->adapter != index) continue;
            if (start < b->offset + b->size && b->offset < start + size) { clash = true; break; }
        }
        if (!clash) best = start;
    }
    if (best != GPU_VRAM_INVALID) {
        int slot = -1;
        for (int i = 0; i < GPU_VRAM_MAX_BLOCKS; i++) if (!g_blocks[i].used) { slot = i; break; }
        if (slot < 0) {
            best = GPU_VRAM_INVALID;
        } else {
            g_blocks[slot] = (vram_block_t){ true, best, size, owner_pid, index };
        }
    }
    spinlock_release(&g_vram_lock, flags);
    return best;
}

void gpu_vram_free(gpu_adapter_t* adapter, uint64_t offset) {
    if (!adapter) return;
    uint32_t index = (uint32_t)(adapter - g_adapters);
    uint64_t flags = spinlock_acquire(&g_vram_lock);
    for (int i = 0; i < GPU_VRAM_MAX_BLOCKS; i++) {
        if (g_blocks[i].used && g_blocks[i].adapter == index && g_blocks[i].offset == offset) {
            g_blocks[i].used = false;
            break;
        }
    }
    spinlock_release(&g_vram_lock, flags);
}

void gpu_vram_free_owner(uint64_t owner_pid) {
    uint64_t flags = spinlock_acquire(&g_vram_lock);
    for (int i = 0; i < GPU_VRAM_MAX_BLOCKS; i++) {
        if (g_blocks[i].used && g_blocks[i].owner == owner_pid) g_blocks[i].used = false;
    }
    spinlock_release(&g_vram_lock, flags);
}

uint64_t gpu_vram_used(const gpu_adapter_t* adapter) {
    if (!adapter) return 0;
    uint32_t index = (uint32_t)(adapter - g_adapters);
    uint64_t used = 0;
    for (int i = 0; i < GPU_VRAM_MAX_BLOCKS; i++) {
        if (g_blocks[i].used && g_blocks[i].adapter == index) used += g_blocks[i].size;
    }
    return used;
}

bool gpu_vram_probe_range(gpu_adapter_t* adapter, uint64_t offset, uint64_t length) {
    if (!adapter || !adapter->aperture || length < 8) return false;
    if (offset + length > adapter->aperture_size) return false;
    // Two words at each end of the range.
    volatile uint32_t* a = (volatile uint32_t*)(adapter->aperture + offset);
    volatile uint32_t* b = (volatile uint32_t*)(adapter->aperture + offset + length - 8);
    uint32_t sa0 = a[0], sa1 = a[1], sb0 = b[0], sb1 = b[1];
    a[0] = 0x4D4F5331u; a[1] = 0xA5C3E1F0u;
    b[0] = 0x0F1E3C5Au; b[1] = 0x31534F4Du;
    __asm__ volatile("sfence" ::: "memory");
    bool ok = a[0] == 0x4D4F5331u && a[1] == 0xA5C3E1F0u &&
              b[0] == 0x0F1E3C5Au && b[1] == 0x31534F4Du;
    a[0] = sa0; a[1] = sa1; b[0] = sb0; b[1] = sb1;
    __asm__ volatile("sfence" ::: "memory");
    return ok;
}

/* ---------------------------------------------------------------- setup */

static void link_scanout(gpu_adapter_t* a) {
    gpu_device_t* screen = getSystemGPU();
    if (!screen || !screen->fb || !screen->fb_phys || !a->aperture_phys) return;
    uint64_t size = (uint64_t)screen->pitch * screen->height;
    if (screen->fb_phys < a->aperture_phys ||
        screen->fb_phys + size > a->aperture_phys + a->aperture_size) return;
    a->scanout_in_vram = true;
    a->scanout_offset = screen->fb_phys - a->aperture_phys;
    a->scanout_size = size;
    a->scanout_pitch = screen->pitch;
    a->caps |= GPU_CAP_SCANOUT;
}

// Default heap: everything in the aperture after the visible framebuffer,
// minus a reserve at the end (firmware may keep data at the top of VRAM).
static void default_heap(gpu_adapter_t* a, uint64_t reserve_end) {
    if (!a->aperture || !a->aperture_size) return;
    uint64_t start = a->scanout_in_vram ? a->scanout_offset + a->scanout_size : 0;
    start = (start + 0xFFFFF) & ~0xFFFFFull;    // 1 MiB aligned
    uint64_t end = a->aperture_size;
    if (a->vram_size && a->vram_size < end) end = a->vram_size;
    end = end > reserve_end ? end - reserve_end : 0;
    if (end <= start + 0x100000) return;
    a->heap_start = start;
    a->heap_end = end;
}

void gpu_core_init(void) {
    extern pci_device_t pci_devices[];
    extern int pci_device_count;

    for (int i = 0; i < pci_device_count && g_adapter_count < GPU_MAX_ADAPTERS; i++) {
        pci_device_t* pci = &pci_devices[i];
        if (pci->class_code != 0x03) continue;
        gpu_adapter_t* a = &g_adapters[g_adapter_count];
        for (uint32_t d = 0; d < sizeof(g_drivers) / sizeof(g_drivers[0]); d++) {
            memset(a, 0, sizeof(*a));
            a->pci = pci;
            if (!g_drivers[d]->probe(a, pci)) continue;
            a->present = true;
            strncpy(a->driver, g_drivers[d]->name, sizeof(a->driver) - 1);
            break;
        }
        if (!a->present) continue;

        if (!a->scanout_in_vram) link_scanout(a);
        if ((a->caps & GPU_CAP_VRAM_HEAP) && a->heap_end <= a->heap_start) {
            default_heap(a, a->vendor == GPU_VENDOR_QEMU ? 0 : 16ull * 1024 * 1024);
        }
        // Never hand out VRAM that does not behave like memory.
        if ((a->caps & GPU_CAP_VRAM_HEAP) &&
            (a->heap_end <= a->heap_start ||
             !gpu_vram_probe_range(a, a->heap_start, a->heap_end - a->heap_start))) {
            a->caps &= ~GPU_CAP_VRAM_HEAP;
            a->heap_start = a->heap_end = 0;
            serial_write_str("gpu: VRAM heap disabled (window did not read back)\n");
        }
        if (a->scanout_in_vram && g_primary < 0) g_primary = (int)g_adapter_count;

        serial_write_str("gpu: ");
        serial_write_str(a->name);
        serial_write_str(" [");
        serial_write_str(a->driver);
        serial_write_str("] VRAM ");
        serial_write_dec(a->vram_size >> 20);
        serial_write_str(" MiB, heap ");
        serial_write_dec((a->heap_end - a->heap_start) >> 20);
        serial_write_str(" MiB\n");
        g_adapter_count++;
    }
    if (g_adapter_count == 0) serial_write_str("gpu: no display adapter found\n");
    gpu_core_place_backbuffer();
    fgpu_backend_select();
}

/* Kernel back buffer in VRAM */

extern bool graphics_use_backbuffer_memory(uint32_t* memory, size_t bytes);

bool gpu_core_place_backbuffer(void) {
    gpu_adapter_t* a = gpu_primary_adapter();
    gpu_device_t* screen = getSystemGPU();
    if (!a || !screen || !(a->caps & GPU_CAP_VRAM_FAST_READ) ||
        !(a->caps & GPU_CAP_VRAM_HEAP)) return false;
    uint64_t bytes = (uint64_t)screen->width * screen->height * 4;
    uint64_t off = gpu_vram_alloc(a, bytes, 4096, 0);
    if (off == GPU_VRAM_INVALID) return false;
    if (!graphics_use_backbuffer_memory((uint32_t*)gpu_vram_ptr(a, off), (size_t)bytes)) {
        gpu_vram_free(a, off);
        return false;
    }
    serial_write_str("gpu: screen back buffer now lives in VRAM\n");
    return true;
}
