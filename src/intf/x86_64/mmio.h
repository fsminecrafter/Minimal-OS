#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "x86_64/pci.h"

// Initialize MMIO subsystem
void mmio_init(void);

// Allocate virtual address space for MMIO mapping
void* mmio_alloc_va(size_t size);

// Map physical address to virtual address with specified size
bool map_mmio_page(uintptr_t virt_addr, uintptr_t phys_addr, size_t size);

// Map a PCI BAR to virtual memory
volatile void* pci_map_bar_mmio(pci_device_t* dev, int bar_num);

// Get the size of a PCI BAR
size_t pci_get_bar_size(pci_device_t* dev, int bar_num);

#endif // MMIO_H