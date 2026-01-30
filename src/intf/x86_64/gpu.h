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
} gpu_device_t;

bool gpu_init(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height);
void gpu_put_pixel(gpu_device_t* gpu, uint32_t x, uint32_t y, uint32_t color);
void gpu_clear(gpu_device_t* gpu, uint32_t color);
void gpu_test(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height);