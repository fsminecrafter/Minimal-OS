#ifndef PCI_WRITE_H
#define PCI_WRITE_H

#include <stdint.h>

// PCI config address and data ports
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#ifdef __cplusplus
extern "C" {
#endif

// Construct PCI config address for bus/device/function/register
static inline uint32_t pci_config_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return (uint32_t)(
        (1U << 31)               | // Enable bit
        ((uint32_t)bus << 16)    |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8)|
        (offset & 0xFC)            // Align offset to 4 bytes (DWORD aligned)
    );
}

// I/O port access (platform-specific implementations)
// These use GCC inline asm for x86_64

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// PCI config space reads/writes - implemented as static inline

static inline uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA + (offset & 0x3));
}

static inline void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA + (offset & 0x3), value);
}

static inline uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inw(PCI_CONFIG_DATA + (offset & 0x2));
}

static inline void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    uint32_t address = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outw(PCI_CONFIG_DATA + (offset & 0x2), value);
}

static inline uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inb(PCI_CONFIG_DATA + (offset & 0x3));
}

static inline void pci_config_write8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value) {
    uint32_t address = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outb(PCI_CONFIG_DATA + (offset & 0x3), value);
}

static inline uint64_t pci_config_read64(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t low = pci_config_read32(bus, device, function, offset);
    uint32_t high = pci_config_read32(bus, device, function, offset + 4);
    return ((uint64_t)high << 32) | low;
}

static inline void pci_config_write64(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint64_t value) {
    pci_config_write32(bus, device, function, offset, (uint32_t)(value & 0xFFFFFFFF));
    pci_config_write32(bus, device, function, offset + 4, (uint32_t)(value >> 32));
}

#ifdef __cplusplus
}
#endif

#endif // PCI_WRITE_H