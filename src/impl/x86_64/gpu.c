#include "x86_64/gpu.h"
#include "serial.h"
#include "x86_64/mmio.h"
#include "panic.h"
#include "x86_64/port.h"
#include "graphics.h"

#define GPU_FB_BAR_INDEX 0
#define GPU_REG_BAR_INDEX 2

// Bochs VBE (VESA BIOS Extensions) registers
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID              0x0
#define VBE_DISPI_INDEX_XRES            0x1
#define VBE_DISPI_INDEX_YRES            0x2
#define VBE_DISPI_INDEX_BPP             0x3
#define VBE_DISPI_INDEX_ENABLE          0x4
#define VBE_DISPI_INDEX_BANK            0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x7
#define VBE_DISPI_INDEX_X_OFFSET        0x8
#define VBE_DISPI_INDEX_Y_OFFSET        0x9

#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_LFB_ENABLED           0x40

static void vbe_write(uint16_t index, uint16_t value) {
    port_outw(VBE_DISPI_IOPORT_INDEX, index);
    port_outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t vbe_read(uint16_t index) {
    port_outw(VBE_DISPI_IOPORT_INDEX, index);
    return port_inw(VBE_DISPI_IOPORT_DATA);
}

static void gpu_set_vbe_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    serial_write_str("Setting VBE mode: ");
    serial_write_dec(width);
    serial_write_str("x");
    serial_write_dec(height);
    serial_write_str("x");
    serial_write_dec(bpp);
    serial_write_str("\n");
    
    // Disable VBE
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    
    // Set resolution
    vbe_write(VBE_DISPI_INDEX_XRES, width);
    vbe_write(VBE_DISPI_INDEX_YRES, height);
    vbe_write(VBE_DISPI_INDEX_BPP, bpp);
    
    // Enable VBE with linear framebuffer
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    
    // Verify settings
    uint16_t set_width = vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t set_height = vbe_read(VBE_DISPI_INDEX_YRES);
    uint16_t set_bpp = vbe_read(VBE_DISPI_INDEX_BPP);
    
    serial_write_str("VBE mode set successfully: ");
    serial_write_dec(set_width);
    serial_write_str("x");
    serial_write_dec(set_height);
    serial_write_str("x");
    serial_write_dec(set_bpp);
    serial_write_str("\n");
}

bool gpu_init(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height) {
    if (!pci_dev) {
        serial_write_str("gpu_init: PCI device NULL\n");
        return false;
    }

    serial_write_str("gpu_init: Initializing GPU...\n");
    
    // Set VBE mode first (before mapping framebuffer)
    gpu_set_vbe_mode(width, height, 32);

    serial_write_str("gpu_init: mapping framebuffer BAR0...\n");

    // Map framebuffer BAR0
    size_t fb_size = pci_get_bar_size(pci_dev, GPU_FB_BAR_INDEX);
    uintptr_t fb_phys = pci_dev->bar[GPU_FB_BAR_INDEX] & ~0xFULL;
    
    serial_write_str("Requesting VA for framebuffer size: ");
    serial_write_hex(fb_size);
    serial_write_str("\n");
    
    void* fb_va_ptr = mmio_alloc_va(fb_size);
    
    serial_write_str("mmio_alloc_va returned ptr: ");
    serial_write_hex((uintptr_t)fb_va_ptr);
    serial_write_str("\n");
    
    uintptr_t fb_va = (uintptr_t)fb_va_ptr;
    
    serial_write_str("After cast to uintptr_t: ");
    serial_write_hex(fb_va);
    serial_write_str("\n");
    
    if (fb_va == 0) {
        PANIC("gpu_init: mmio_alloc_va returned NULL for framebuffer");
    }

    serial_write_str("Attempting to map framebuffer...\n");
    
    if (!map_mmio_page(fb_va, fb_phys, fb_size)) {
        PANIC("gpu_init: failed to map framebuffer BAR0");
    }

    serial_write_str("Framebuffer BAR0 physical: ");
    serial_write_hex(fb_phys);
    serial_write_str(", virtual: ");
    serial_write_hex(fb_va);
    serial_write_str(", size: ");
    serial_write_hex(fb_size);
    serial_write_str("\n");

    // Map BAR2 (registers) if present
    uintptr_t reg_va = 0;
    size_t reg_size = pci_get_bar_size(pci_dev, GPU_REG_BAR_INDEX);
    uintptr_t reg_phys = pci_dev->bar[GPU_REG_BAR_INDEX] & ~0xFULL;

    if (reg_size > 0 && pci_dev->bar_type[GPU_REG_BAR_INDEX] == PCI_BAR_MEM) {
        serial_write_str("gpu_init: mapping BAR2 (registers)...\n");
        serial_write_str("BAR2 size: ");
        serial_write_hex(reg_size);
        serial_write_str("\n");
        
        reg_va = (uintptr_t)mmio_alloc_va(reg_size);
        
        serial_write_str("mmio_alloc_va returned for BAR2: ");
        serial_write_hex(reg_va);
        serial_write_str("\n");
        
        if (reg_va == 0) {
            serial_write_str("Warning: mmio_alloc_va returned NULL for BAR2, skipping\n");
        } else {
            serial_write_str("Attempting to map BAR2...\n");
            
            if (!map_mmio_page(reg_va, reg_phys, reg_size)) {
                serial_write_str("Warning: failed to map BAR2 registers, continuing anyway\n");
                reg_va = 0;
            } else {
                serial_write_str("GPU BAR2 mapped safely: physical ");
                serial_write_hex(reg_phys);
                serial_write_str(", virtual ");
                serial_write_hex(reg_va);
                serial_write_str(", size ");
                serial_write_hex(reg_size);
                serial_write_str("\n");
            }
        }
    } else {
        serial_write_str("GPU BAR2 not present or not MMIO\n");
    }

    // Fill GPU struct
    gpu->pci_dev = pci_dev;
    gpu->fb = (volatile uint32_t*)fb_va;
    gpu->width = width;
    gpu->height = height;
    gpu->bpp = 4;
    gpu->pitch = width * gpu->bpp;

    serial_write_str("GPU initialized: resolution ");
    serial_write_dec(width);
    serial_write_str("x");
    serial_write_dec(height);
    serial_write_str(", pitch ");
    serial_write_dec(gpu->pitch);
    serial_write_str("\n");

    // Clear screen safely
    serial_write_str("Clearing framebuffer...\n");
    gpu_clear(gpu, 0x00000000);
    serial_write_str("Framebuffer cleared\n");

    return true;
}


void gpu_put_pixel(gpu_device_t* gpu, uint32_t x, uint32_t y, uint32_t color) {
    if (!gpu || !gpu->fb) {
        return;
    }
    if (x >= gpu->width || y >= gpu->height) {
        return;
    }

    uint32_t offset = (y * gpu->pitch + x * gpu->bpp) / 4;
    volatile uint32_t* pixel = gpu->fb + offset;
    *pixel = color;
}

void gpu_clear(gpu_device_t* gpu, uint32_t color) {
    if (!gpu || !gpu->fb) PANIC("gpu_clear: framebuffer NULL");

    serial_write_str("gpu_clear: clearing ");
    serial_write_dec(gpu->width);
    serial_write_str("x");
    serial_write_dec(gpu->height);
    serial_write_str(" framebuffer with color ");
    serial_write_hex(color);
    serial_write_str("\n");

    for (size_t y = 0; y < gpu->height; y++) {
        volatile uint32_t* row = (volatile uint32_t*)((uintptr_t)gpu->fb + y * gpu->pitch);
        for (size_t x = 0; x < gpu->width; x++) {
            row[x] = color;
        }
    }

    serial_write_str("gpu_clear: done\n");
}

void gpu_test_write(gpu_device_t* gpu) {
    if (!gpu || !gpu->fb) return;

    serial_write_str("gpu_test_write: drawing color gradient...\n");

    // Create a smooth color gradient
    // Horizontal gradient: Red -> Yellow -> Green -> Cyan -> Blue -> Magenta -> Red
    // Vertical gradient: Bright at top -> Dark at bottom
    
    for (uint32_t y = 0; y < gpu->height; y++) {
        volatile uint32_t* row = (volatile uint32_t*)((uintptr_t)gpu->fb + y * gpu->pitch);
        
        // Vertical brightness factor (0.0 to 1.0)
        uint32_t brightness = (255 * (gpu->height - y)) / gpu->height;
        
        for (uint32_t x = 0; x < gpu->width; x++) {
            // Horizontal hue cycle (0 to 6 across width)
            uint32_t hue_section = (x * 6) / gpu->width;
            uint32_t hue_fraction = ((x * 6 * 255) / gpu->width) % 255;
            
            uint32_t r, g, b;
            
            // RGB color wheel
            switch (hue_section) {
                case 0: // Red -> Yellow
                    r = 255;
                    g = hue_fraction;
                    b = 0;
                    break;
                case 1: // Yellow -> Green
                    r = 255 - hue_fraction;
                    g = 255;
                    b = 0;
                    break;
                case 2: // Green -> Cyan
                    r = 0;
                    g = 255;
                    b = hue_fraction;
                    break;
                case 3: // Cyan -> Blue
                    r = 0;
                    g = 255 - hue_fraction;
                    b = 255;
                    break;
                case 4: // Blue -> Magenta
                    r = hue_fraction;
                    g = 0;
                    b = 255;
                    break;
                default: // Magenta -> Red
                    r = 255;
                    g = 0;
                    b = 255 - hue_fraction;
                    break;
            }
            
            // Apply vertical brightness
            r = (r * brightness) / 255;
            g = (g * brightness) / 255;
            b = (b * brightness) / 255;
            
            // Pack into ARGB format (0xAARRGGBB)
            row[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
        
        // Progress indicator every 100 rows
        if (y % 100 == 0) {
            serial_write_str("  Row ");
            serial_write_dec(y);
            serial_write_str("/");
            serial_write_dec(gpu->height);
            serial_write_str("\n");
        }
    }

    serial_write_str("gpu_test_write: gradient complete\n");
}

void gpu_test(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height) {
    serial_write_str("gpu_test: starting GPU test...\n");
    gpu_init(gpu, pci_dev, width, height);
    serial_write_str("gpu_test: GPU initialized, writing test pixels...\n");
    graphics_safety_mode = true; // Enable safety mode for testing
    gpu_test_write(gpu);
    gpu_clear(gpu, 0x00000000); 
    graphics_complete_demo(gpu);
    serial_write_str("gpu_test: done\n");
}

void gpu_initialize_g(gpu_device_t* gpu, pci_device_t* pci_dev, uint32_t width, uint32_t height) {
    serial_write_str("gpu_init: starting GPU...\n");
    gpu_init(gpu, pci_dev, width, height);
    serial_write_str("gpu_init: GPU initialized, writing test pixels...\n");
    graphics_safety_mode = true; // Enable safety mode for testing
    gpu_test_write(gpu);
    gpu_clear(gpu, 0x00000000);
    graphics_set_gpu(gpu);
    graphics_init();
    serial_write_str("gpu_init: done\n");
}
