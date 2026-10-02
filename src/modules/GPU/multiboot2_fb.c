#include <stdint.h>
#include <stddef.h>
#include "graphics.h"
#include "prochandler.h"
#include "serial.h"
#include "x86_64/gpu.h"
#include "x86_64/gpu_hw.h"
#include "x86_64/mmio.h"
#include "x86_64/multiboot2parse.h"

#define FRAMEBUFFER_MAX_BYTES (128ULL * 1024 * 1024)

static multiboot2_info_t* g_multiboot_info;
static gpu_device_t g_framebuffer_gpu;

void gpu_multiboot2_set_info(multiboot2_info_t* info) {
    g_multiboot_info = info;
}

static bool framebuffer_channel_valid(uint8_t position, uint8_t size) {
    return size > 0 && size <= 16 && position + size <= 32;
}

static bool framebuffer_init(void) {
    multiboot2_framebuffer_info_t info;
    if (!multiboot2_get_framebuffer(g_multiboot_info, &info)) {
        serial_write_str("GPU (Multiboot2): no RGB framebuffer tag\n");
        return false;
    }

    if (info.framebuffer_type != 1 || info.bpp != 32 ||
        info.width == 0 || info.height == 0 ||
        info.width > 8192 || info.height > 8192 ||
        info.pitch < info.width * 4U || (info.pitch & 3U) != 0 ||
        !framebuffer_channel_valid(info.red_position, info.red_mask_size) ||
        !framebuffer_channel_valid(info.green_position, info.green_mask_size) ||
        !framebuffer_channel_valid(info.blue_position, info.blue_mask_size)) {
        serial_write_str("GPU (Multiboot2): unsupported framebuffer layout\n");
        return false;
    }

    uint32_t red_mask = ((1U << info.red_mask_size) - 1U) << info.red_position;
    uint32_t green_mask = ((1U << info.green_mask_size) - 1U) << info.green_position;
    uint32_t blue_mask = ((1U << info.blue_mask_size) - 1U) << info.blue_position;
    if ((red_mask & green_mask) || (red_mask & blue_mask) ||
        (green_mask & blue_mask)) {
        serial_write_str("GPU (Multiboot2): overlapping RGB channels\n");
        return false;
    }

    uint64_t framebuffer_size = (uint64_t)info.pitch * info.height;
    if (framebuffer_size == 0 || framebuffer_size > FRAMEBUFFER_MAX_BYTES ||
        info.address > UINT64_MAX - framebuffer_size) {
        serial_write_str("GPU (Multiboot2): framebuffer range is invalid\n");
        return false;
    }

    uintptr_t physical_base = (uintptr_t)info.address & ~0xFFFULL;
    size_t physical_offset = (size_t)(info.address - physical_base);
    if (framebuffer_size > SIZE_MAX - physical_offset - 0xFFF) return false;
    size_t mapping_size = (physical_offset + (size_t)framebuffer_size + 0xFFF) &
                          ~(size_t)0xFFF;
    void* virtual_base = mmio_alloc_va(mapping_size);
    if (!virtual_base || !map_mmio_page_cached((uintptr_t)virtual_base,
                                                physical_base, mapping_size)) {
        serial_write_str("GPU (Multiboot2): framebuffer mapping failed\n");
        return false;
    }

    g_framebuffer_gpu = (gpu_device_t){
        .pci_dev = NULL,
        .fb = (volatile uint32_t*)((uintptr_t)virtual_base + physical_offset),
        .width = info.width,
        .height = info.height,
        .pitch = info.pitch,
        .bpp = 4,
        .red_position = info.red_position,
        .red_mask_size = info.red_mask_size,
        .green_position = info.green_position,
        .green_mask_size = info.green_mask_size,
        .blue_position = info.blue_position,
        .blue_mask_size = info.blue_mask_size,
    };

    gpu_clear(&g_framebuffer_gpu, 0xFF000000);
    graphics_set_gpu(&g_framebuffer_gpu);
    setSystemGPU(&g_framebuffer_gpu);
    graphics_init();

    serial_write_str("GPU (Multiboot2): framebuffer active ");
    serial_write_dec(info.width);
    serial_write_str("x");
    serial_write_dec(info.height);
    serial_write_str(" pitch=");
    serial_write_dec(info.pitch);
    serial_write_str("\n");
    return true;
}

static const gpu_hw_driver_t g_multiboot_framebuffer_driver = {
    .name = "Multiboot2 framebuffer",
    .init = framebuffer_init,
};

const gpu_hw_driver_t* gpu_multiboot2_get_driver(void) {
    return &g_multiboot_framebuffer_driver;
}