#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pci.h"

typedef struct {
    pci_device_t* pci_dev;    // The PCI device info
    volatile uint32_t* fb;    // Framebuffer base pointer (MMIO)
    uint32_t width;
    uint32_t height;
    uint32_t pitch;           // bytes per row
    uint8_t bpp;              // bytes per pixel (likely 4 for 32-bit)
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
} gpu_device_t;

extern gpu_device_t gpu;

#define GPU_DEFAULT_WIDTH  1280
#define GPU_DEFAULT_HEIGHT 720

bool gpu_init(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height);
bool gpu_set_resolution(gpu_device_t* gpu, uint32_t width, uint32_t height);
void gpu_put_pixel(gpu_device_t* gpu, uint32_t x, uint32_t y, uint32_t color);
uint32_t gpu_pack_color(const gpu_device_t* gpu, uint32_t color);
void gpu_clear(gpu_device_t* gpu, uint32_t color);
void gpu_test(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height);
void gpu_initialize_g(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height);