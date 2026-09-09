#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/multiboot2parse.h"

#define ACPI_MAX_CPUS      16
#define ACPI_MAX_IOAPICS   4

typedef struct {
    uint32_t apic_id;
    uint32_t acpi_processor_id;
    bool     enabled;
} acpi_cpu_t;

typedef struct {
    uint32_t  id;
    uint32_t  gsi_base;
    uintptr_t phys_addr;
} acpi_ioapic_t;

// Parses the multiboot2 RSDP tag -> RSDT/XSDT -> MADT. Must be called
// after paging is live (any time after long_mode_start) - ACPI tables
// live in low physical memory, which is identity-mapped via
// page_table_l2's 2MB huge pages in main.asm, so physical == virtual
// here and no special mapping is needed to read them.
bool acpi_init(multiboot2_info_t* mb_info);

uintptr_t acpi_get_lapic_phys_addr(void);

uint32_t          acpi_get_cpu_count(void);
const acpi_cpu_t* acpi_get_cpu(uint32_t index);

uint32_t             acpi_get_ioapic_count(void);
const acpi_ioapic_t* acpi_get_ioapic(uint32_t index);

#endif // ACPI_H