#include <stdint.h>
#include <stdbool.h>
#include "serial.h"
#include "x86_64/pci.h"
#include "x86_64/gpu.h"
#include "x86_64/mmio.h"
#include "x86_64/port.h"
#include "x86_64/allocator.h"
#include "graphics.h"

#define MMIO_PAGE_SIZE  0x200000ULL  // 2 MiB
#define MMIO_VA_ALIGN   MMIO_PAGE_SIZE
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PAGE_SIZE 0x1000

// --- Function prototypes for pci config access ---
uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_write_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

// Align helpers
static uintptr_t align_down(uintptr_t addr, uintptr_t align) {
    return addr & ~(align - 1);
}

static uintptr_t align_up(uintptr_t addr, uintptr_t align) {
    return (addr + align - 1) & ~(align - 1);
}

// Write PCI config address port
static inline void pci_write_address(uint32_t address) {
    port_outl(PCI_CONFIG_ADDRESS, address);
}

// --- PCI config dword read ---
uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    if (offset & 3) return 0xFFFFFFFF; // must be dword aligned

    uint32_t address = (1U << 31)
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)device << 11)
                     | ((uint32_t)function << 8)
                     | (offset & 0xFC);

    pci_write_address(address);
    return port_inl(PCI_CONFIG_DATA);
}

// --- PCI config dword write ---
void pci_write_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    if (offset & 3) return;

    uint32_t address = (1U << 31)
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)device << 11)
                     | ((uint32_t)function << 8)
                     | (offset & 0xFC);

    pci_write_address(address);
    port_outl(PCI_CONFIG_DATA, value);
}

// --- pci_read_data already replaced by port_inl above; can remove redundant functions ---

uint32_t pci_read_data(void) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(PCI_CONFIG_DATA));
    return ret;
}

#define MAX_PCI_DEVICES 64
pci_device_t pci_devices[MAX_PCI_DEVICES];
int pci_device_count = 0;

bool pci_device_present(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor = pci_config_read_word(bus, device, function, 0x00);
    return vendor != 0xFFFF;
}

static inline void mmio_write32(volatile void* base, uint32_t offset, uint32_t value) {
    *((volatile uint32_t*)((uintptr_t)base + offset)) = value;
}

static inline uint32_t mmio_read32(volatile void* base, uint32_t offset) {
    return *((volatile uint32_t*)((uintptr_t)base + offset));
}

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    // Align offset to 4 bytes
    uint32_t address = (1U << 31)               // Enable bit
        | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11)
        | ((uint32_t)function << 8)
        | (offset & 0xFC);                      // Offset lower 2 bits zeroed
    
    pci_write_address(address);
    return pci_read_data();
}

uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t val = pci_config_read(bus, device, function, offset & 0xFC);
    uint16_t word;
    // Extract word based on offset mod 4
    uint8_t pos = offset & 2;
    if (pos == 0)
        word = val & 0xFFFF;
    else
        word = (val >> 16) & 0xFFFF;
    return word;
}

void pci_enumerate_all() {
    serial_write_str("Enumerating PCI devices starting from bus 0...\n");
    pci_enumerate_bus(0);
}

void pci_enumerate_bus(uint8_t bus) {
    serial_write_str("Enumerating bus: ");
    serial_write_dec(bus);
    serial_write_str("\n");

    for (uint8_t device = 0; device < 32; device++) {
        if (!pci_device_present(bus, device, 0)) continue;

        uint8_t header_type = (pci_config_read(bus, device, 0, 0x0C) >> 16) & 0xFF;
        uint8_t functions = (header_type & 0x80) ? 8 : 1;

        for (uint8_t function = 0; function < functions; function++) {
            if (!pci_device_present(bus, device, function)) continue;

            if (pci_device_count >= MAX_PCI_DEVICES) {
                serial_write_str("PCI device array full\n");
                return;
            }

            uint16_t vendor_id = pci_config_read_word(bus, device, function, 0x00);
            uint16_t device_id = pci_config_read_word(bus, device, function, 0x02);
            uint32_t class_reg = pci_config_read(bus, device, function, 0x08);
            uint8_t class_code = (class_reg >> 24) & 0xFF;
            uint8_t subclass = (class_reg >> 16) & 0xFF;
            uint8_t prog_if = (class_reg >> 8) & 0xFF;

            pci_device_t* dev = &pci_devices[pci_device_count++];
            dev->bus = bus;
            dev->device = device;
            dev->function = function;
            dev->vendor_id = vendor_id;
            dev->device_id = device_id;
            dev->class_code = class_code;
            dev->subclass = subclass;
            dev->prog_if = prog_if;
            dev->header_type = header_type;

            // Read BARs
            for (int bar_num = 0; bar_num < PCI_NUM_BARS; bar_num++) {
                uint32_t bar_val = pci_config_read(bus, device, function, 0x10 + bar_num * 4);
                dev->bar[bar_num] = bar_val;

                if (bar_val == 0) {
                    dev->bar_type[bar_num] = PCI_BAR_UNUSED;
                } else if (bar_val & 0x1) {
                    dev->bar_type[bar_num] = PCI_BAR_IO;
                    dev->bar[bar_num] = bar_val & 0xFFFFFFFC;
                } else {
                    dev->bar_type[bar_num] = PCI_BAR_MEM;
                    dev->bar[bar_num] = bar_val & 0xFFFFFFF0;
                }
            }

            // Handle PCI-to-PCI bridge: recursively enumerate secondary bus
            if (class_code == 0x06 && subclass == 0x04) {
                uint8_t secondary_bus = (pci_config_read(bus, device, function, 0x18) >> 8) & 0xFF;
                if (secondary_bus != 0 && secondary_bus != bus) {
                    pci_enumerate_bus(secondary_bus);
                }
            }
        }
    }
}

gpu_device_t gpu;

void test_gpu() {
    // Find first GPU device (class code 0x03 = display controller)
    pci_device_t* gpu_pci = NULL;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == 0x03) {
            gpu_pci = &pci_devices[i];
            break;
        }
    }

    if (!gpu_pci) {
        serial_write_str("No GPU device found.\n");
        return;
    }

    gpu_test(&gpu, gpu_pci, 1080, 720);

    // Draw a red pixel at (10,10)

    // Fill screen with green after 2 seconds (example)
    // (Assuming you have some timer/delay function)
    // delay(2000);
    // gpu_clear(&gpu, 0x0000FF00);
}

void initializeGraphicsDevice() {
    pci_device_t* gpu_pci = NULL;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == 0x03) {
            gpu_pci = &pci_devices[i];
            break;
        }
    }

    if (!gpu_pci) {
        serial_write_str("No GPU device found.\n");
        return;
    }

    gpu_initialize_g(&gpu, gpu_pci, 1080, 720);
}

void print_pci_devices() {
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* dev = &pci_devices[i];
        serial_write_str("PCI Device found: ");
        serial_write_hex(dev->bus);
        serial_write(':');
        serial_write_hex(dev->device);
        serial_write(':');
        serial_write_hex(dev->function);
        serial_write_str(" Vendor: ");
        serial_write_hex(dev->vendor_id);
        serial_write_str(" Device: ");
        serial_write_hex(dev->device_id);
        serial_write_str("\n");
    }
    serial_write_dec(pci_device_count);
}
