#include "x86_64/acpi.h"
#include "serial.h"
#include "string.h"

#define MB2_TAG_ACPI_OLD_RSDP 14
#define MB2_TAG_ACPI_NEW_RSDP 15

typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    // ACPI 2.0+ only - valid iff revision >= 2
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t local_apic_addr;
    uint32_t flags;
    uint8_t  entries[];
} acpi_madt_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} acpi_madt_entry_header_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} acpi_madt_lapic_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} acpi_madt_ioapic_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t header;
    uint16_t reserved;
    uint64_t lapic_addr;
} acpi_madt_lapic_override_t;

#define ACPI_MADT_TYPE_LAPIC          0
#define ACPI_MADT_TYPE_IOAPIC         1
#define ACPI_MADT_TYPE_LAPIC_OVERRIDE 5
#define ACPI_MADT_LAPIC_ENABLED       (1 << 0)

static uintptr_t     g_lapic_phys_addr = 0xFEE00000; // xAPIC default
static acpi_cpu_t     g_cpus[ACPI_MAX_CPUS];
static uint32_t       g_cpu_count = 0;
static acpi_ioapic_t  g_ioapics[ACPI_MAX_IOAPICS];
static uint32_t       g_ioapic_count = 0;

static uint8_t acpi_checksum(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    return sum;
}

// Walks the multiboot2 tag list exactly like get_total_memory() does
// in multiboot2parse.c, looking for the ACPI RSDP tags instead of the
// memory map. Prefers the ACPI 2.0+ (new) RSDP when both are present,
// since only it has an XSDT pointer.
static const acpi_rsdp_t* find_mb2_rsdp(multiboot2_info_t* mb_info) {
    if (!mb_info) return NULL;

    uint8_t* ptr = mb_info->tags;
    uint8_t* end = (uint8_t*)mb_info + mb_info->total_size;

    const acpi_rsdp_t* new_rsdp = NULL;
    const acpi_rsdp_t* old_rsdp = NULL;

    while (ptr < end) {
        multiboot2_tag_t* tag = (multiboot2_tag_t*)ptr;
        if (tag->type == 0) break;

        if (tag->type == MB2_TAG_ACPI_NEW_RSDP) {
            new_rsdp = (const acpi_rsdp_t*)(ptr + 8);
        } else if (tag->type == MB2_TAG_ACPI_OLD_RSDP) {
            old_rsdp = (const acpi_rsdp_t*)(ptr + 8);
        }

        ptr += (tag->size + 7) & ~7;
    }

    return new_rsdp ? new_rsdp : old_rsdp;
}

static const acpi_sdt_header_t* find_table(const acpi_rsdp_t* rsdp, const char* signature) {
    if (!rsdp) return NULL;

    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        const acpi_sdt_header_t* xsdt = (const acpi_sdt_header_t*)(uintptr_t)rsdp->xsdt_address;
        uint32_t count = (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
        const uint64_t* tables = (const uint64_t*)((const uint8_t*)xsdt + sizeof(acpi_sdt_header_t));

        for (uint32_t i = 0; i < count; i++) {
            const acpi_sdt_header_t* t = (const acpi_sdt_header_t*)(uintptr_t)tables[i];
            if (strncmp(t->signature, signature, 4) == 0) return t;
        }
        return NULL;
    }

    if (rsdp->rsdt_address) {
        const acpi_sdt_header_t* rsdt = (const acpi_sdt_header_t*)(uintptr_t)rsdp->rsdt_address;
        uint32_t count = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
        const uint32_t* tables = (const uint32_t*)((const uint8_t*)rsdt + sizeof(acpi_sdt_header_t));

        for (uint32_t i = 0; i < count; i++) {
            const acpi_sdt_header_t* t = (const acpi_sdt_header_t*)(uintptr_t)tables[i];
            if (strncmp(t->signature, signature, 4) == 0) return t;
        }
    }

    return NULL;
}

bool acpi_init(multiboot2_info_t* mb_info) {
    serial_write_str("ACPI: Locating RSDP...\n");

    const acpi_rsdp_t* rsdp = find_mb2_rsdp(mb_info);
    if (!rsdp) {
        // If this fires, your bootloader isn't handing multiboot2 the
        // ACPI tag by default - GRUB2 normally does. header.asm may
        // need an explicit "information request" tag for types 14/15
        // if you're using a minimal/custom loader.
        serial_write_str("ACPI: No RSDP tag from bootloader\n");
        return false;
    }

    if (acpi_checksum(rsdp, 20) != 0) {
        serial_write_str("ACPI: RSDP checksum failed\n");
        return false;
    }
    if (rsdp->revision >= 2 && acpi_checksum(rsdp, rsdp->length) != 0) {
        serial_write_str("ACPI: Extended RSDP checksum failed\n");
        return false;
    }

    const acpi_sdt_header_t* madt_hdr = find_table(rsdp, "APIC");
    if (!madt_hdr) {
        serial_write_str("ACPI: MADT not found\n");
        return false;
    }

    const acpi_madt_t* madt = (const acpi_madt_t*)madt_hdr;
    g_lapic_phys_addr = madt->local_apic_addr;

    const uint8_t* p   = madt->entries;
    const uint8_t* endp = (const uint8_t*)madt + madt->header.length;

    while (p < endp) {
        const acpi_madt_entry_header_t* eh = (const acpi_madt_entry_header_t*)p;
        if (eh->length == 0) break;

        switch (eh->type) {
            case ACPI_MADT_TYPE_LAPIC: {
                const acpi_madt_lapic_t* e = (const acpi_madt_lapic_t*)p;
                if ((e->flags & ACPI_MADT_LAPIC_ENABLED) && g_cpu_count < ACPI_MAX_CPUS) {
                    g_cpus[g_cpu_count].apic_id           = e->apic_id;
                    g_cpus[g_cpu_count].acpi_processor_id = e->acpi_processor_id;
                    g_cpus[g_cpu_count].enabled           = true;
                    g_cpu_count++;
                }
                break;
            }
            case ACPI_MADT_TYPE_IOAPIC: {
                const acpi_madt_ioapic_t* e = (const acpi_madt_ioapic_t*)p;
                if (g_ioapic_count < ACPI_MAX_IOAPICS) {
                    g_ioapics[g_ioapic_count].id        = e->ioapic_id;
                    g_ioapics[g_ioapic_count].gsi_base   = e->gsi_base;
                    g_ioapics[g_ioapic_count].phys_addr  = e->ioapic_addr;
                    g_ioapic_count++;
                }
                break;
            }
            case ACPI_MADT_TYPE_LAPIC_OVERRIDE: {
                const acpi_madt_lapic_override_t* e = (const acpi_madt_lapic_override_t*)p;
                g_lapic_phys_addr = (uintptr_t)e->lapic_addr;
                break;
            }
            default:
                break;
        }

        p += eh->length;
    }

    serial_write_str("ACPI: LAPIC phys=0x");
    serial_write_hex(g_lapic_phys_addr);
    serial_write_str(", CPUs=");
    serial_write_dec(g_cpu_count);
    serial_write_str(", IOAPICs=");
    serial_write_dec(g_ioapic_count);
    serial_write_str("\n");

    return g_cpu_count > 0;
}

uintptr_t acpi_get_lapic_phys_addr(void) { return g_lapic_phys_addr; }
uint32_t  acpi_get_cpu_count(void)       { return g_cpu_count; }

const acpi_cpu_t* acpi_get_cpu(uint32_t index) {
    if (index >= g_cpu_count) return NULL;
    return &g_cpus[index];
}

uint32_t acpi_get_ioapic_count(void) { return g_ioapic_count; }

const acpi_ioapic_t* acpi_get_ioapic(uint32_t index) {
    if (index >= g_ioapic_count) return NULL;
    return &g_ioapics[index];
}