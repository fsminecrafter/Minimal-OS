#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "x86_64/pci.h"

/*
 * GPU core: graphics adapters as more than a framebuffer.
 *
 * Each display-class PCI device gets a gpu_adapter_t from a driver
 * (gpu_bochs.c for QEMU/Bochs, gpu_nvidia.c for GeForce GTX 10 "Pascal" and
 * RTX 20 "Turing" cards, gpu_generic.c for everything else). An adapter
 * knows its video memory: how much there is, the CPU-visible window onto
 * it (a PCI BAR, mapped write-combining) and where the visible
 * framebuffer sits in it. The rest of that window is handed out by a VRAM
 * allocator, so screen buffers and render targets can live in the GPU's
 * own memory.
 *
 * Drawing commands go through the FGPU command processor (fgpu.h), which
 * executes command buffers submitted with SYS_GPU. Its current backend
 * rasterizes on the CPU; the command format does not depend on that, so a
 * hardware backend can take over per adapter later.
 */

#define GPU_MAX_ADAPTERS 4
#define GPU_VRAM_MAX_BLOCKS 128

// Capabilities
#define GPU_CAP_SCANOUT        (1u << 0)   // drives the current display
#define GPU_CAP_VRAM_HEAP      (1u << 1)   // free VRAM can be allocated
#define GPU_CAP_PAGE_FLIP      (1u << 2)   // can show another VRAM buffer
#define GPU_CAP_MODESET        (1u << 3)   // can change the display mode
#define GPU_CAP_VRAM_FAST_READ (1u << 4)   // CPU reads of VRAM are cheap (virtual GPU)
#define GPU_CAP_HW_3D          (1u << 5)   // hardware 3D (no driver has this yet)

typedef enum {
    GPU_VENDOR_OTHER = 0,
    GPU_VENDOR_QEMU,
    GPU_VENDOR_NVIDIA,
    GPU_VENDOR_AMD,
    GPU_VENDOR_INTEL,
    GPU_VENDOR_VMWARE,
    GPU_VENDOR_VIRTIO,
} gpu_vendor_t;

typedef struct gpu_adapter gpu_adapter_t;

struct gpu_adapter {
    bool present;
    char name[64];             // "NVIDIA GeForce RTX 2060"
    char family[40];           // "Turing (TU106)"
    char driver[16];           // "nvidia", "bochs", "generic"
    gpu_vendor_t vendor;
    pci_device_t* pci;
    uint32_t caps;

    uint64_t vram_size;        // total video memory, 0 if unknown
    uint64_t aperture_phys;    // CPU window onto VRAM (PCI BAR)
    uint64_t aperture_size;
    volatile uint8_t* aperture;   // write-combining mapping, NULL if none
    volatile uint32_t* mmio;      // register BAR, NULL if none
    uint64_t mmio_size;

    // The framebuffer the display shows, as an offset into the aperture.
    bool scanout_in_vram;
    uint64_t scanout_offset;
    uint64_t scanout_size;
    uint32_t scanout_pitch;

    // VRAM allocator range (offsets into the aperture).
    uint64_t heap_start;
    uint64_t heap_end;

    // Shows the buffer at offset (page flip). NULL when unsupported.
    bool (*flip)(gpu_adapter_t* adapter, uint64_t offset);

    char notes[96];            // shown by the 'gpu' command
};

typedef struct {
    // Called for each display-class PCI device until one returns true.
    bool (*probe)(gpu_adapter_t* adapter, pci_device_t* pci);
    const char* name;
} gpu_adapter_driver_t;

// Probes every display controller. Call after the display driver
// (gpu_manager) has set the system framebuffer.
void gpu_core_init(void);

uint32_t gpu_adapter_count(void);
gpu_adapter_t* gpu_adapter_get(uint32_t index);
// The adapter driving the screen (or the first one), NULL if none.
gpu_adapter_t* gpu_primary_adapter(void);

// Maps a BAR range write-combining (VRAM) or uncached (registers).
volatile void* gpu_map_bar(pci_device_t* pci, int bar, uint64_t length, bool vram);

/* VRAM allocator. Offsets are into the adapter's aperture. */
#define GPU_VRAM_INVALID UINT64_MAX
uint64_t gpu_vram_alloc(gpu_adapter_t* adapter, uint64_t size, uint64_t align,
                        uint64_t owner_pid);
void gpu_vram_free(gpu_adapter_t* adapter, uint64_t offset);
void gpu_vram_free_owner(uint64_t owner_pid);     // on process exit
uint64_t gpu_vram_used(const gpu_adapter_t* adapter);
static inline void* gpu_vram_ptr(gpu_adapter_t* adapter, uint64_t offset) {
    return (void*)(adapter->aperture + offset);
}

// Moves the kernel's screen back buffer into VRAM when the adapter reads
// VRAM cheaply. Returns true if it moved.
bool gpu_core_place_backbuffer(void);

// Writes a pattern to a VRAM range and reads it back.
bool gpu_vram_probe_range(gpu_adapter_t* adapter, uint64_t offset, uint64_t length);

/* Drivers */
extern const gpu_adapter_driver_t gpu_bochs_adapter_driver;
extern const gpu_adapter_driver_t gpu_nvidia_adapter_driver;
extern const gpu_adapter_driver_t gpu_generic_adapter_driver;
