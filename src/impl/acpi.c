#include "x86_64/acpi.h"
#include "x86_64/port.h"
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

typedef struct __attribute__((packed)) {
    uint8_t address_space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} acpi_gas_t;

#define ACPI_MADT_TYPE_LAPIC          0
#define ACPI_MADT_TYPE_IOAPIC         1
#define ACPI_MADT_TYPE_LAPIC_OVERRIDE 5
#define ACPI_MADT_LAPIC_ENABLED       (1 << 0)

static uintptr_t     g_lapic_phys_addr = 0xFEE00000; // xAPIC default
static acpi_cpu_t     g_cpus[ACPI_MAX_CPUS];
static uint32_t       g_cpu_count = 0;
static acpi_ioapic_t  g_ioapics[ACPI_MAX_IOAPICS];
static uint32_t       g_ioapic_count = 0;
static acpi_gas_t     g_pm1a_control;
static acpi_gas_t     g_pm1b_control;
static acpi_gas_t     g_pm1a_event;
static acpi_gas_t     g_pm1b_event;
static acpi_gas_t     g_reset_register;
static uint16_t       g_smi_command_port;
static uint8_t        g_acpi_enable_value;
static uint8_t        g_reset_value;
static uint8_t        g_s5_type_a;
static uint8_t        g_s5_type_b;
static bool           g_pm1a_available = false;
static bool           g_pm1b_available = false;
static bool           g_pm1a_event_available = false;
static bool           g_pm1b_event_available = false;
static bool           g_reset_available = false;
static bool           g_power_button_available = false;
static bool           g_s5_available = false;

static uint8_t acpi_checksum(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    return sum;
}

static uint16_t acpi_read_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t acpi_read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t acpi_read_u64(const uint8_t* p) {
    return (uint64_t)acpi_read_u32(p) |
           ((uint64_t)acpi_read_u32(p + 4) << 32);
}

static acpi_gas_t acpi_read_gas(const uint8_t* p) {
    acpi_gas_t gas = {
        .address_space_id = p[0],
        .bit_width = p[1],
        .bit_offset = p[2],
        .access_size = p[3],
        .address = acpi_read_u64(p + 4),
    };
    return gas;
}

static bool acpi_gas_valid(const acpi_gas_t* gas, uint8_t min_width) {
    if (!gas || gas->address == 0 || gas->bit_offset != 0 ||
        gas->bit_width < min_width || gas->bit_width > 32) return false;
    if (gas->address_space_id == 1) {
        return gas->address <= 0xFFFF &&
               gas->address + ((gas->bit_width + 7) / 8) <= 0x10000ULL;
    }
    if (gas->address_space_id == 0) {
        return gas->address <= 0x3FFFFFFFULL &&
               gas->address + ((gas->bit_width + 7) / 8) <= 0x40000000ULL;
    }
    return false;
}

static bool acpi_gas_read(const acpi_gas_t* gas, uint32_t* value) {
    if (!value || !acpi_gas_valid(gas, 8)) return false;
    if (gas->address_space_id == 1) {
        if (gas->bit_width <= 8) *value = port_inb((uint16_t)gas->address);
        else if (gas->bit_width <= 16) *value = port_inw((uint16_t)gas->address);
        else *value = port_inl((uint16_t)gas->address);
    } else {
        volatile uint8_t* address = (volatile uint8_t*)(uintptr_t)gas->address;
        if (gas->bit_width <= 8) *value = *(volatile uint8_t*)address;
        else if (gas->bit_width <= 16) *value = *(volatile uint16_t*)address;
        else *value = *(volatile uint32_t*)address;
    }
    return true;
}

static bool acpi_gas_write(const acpi_gas_t* gas, uint32_t value) {
    if (!acpi_gas_valid(gas, 8)) return false;
    if (gas->address_space_id == 1) {
        if (gas->bit_width <= 8) port_outb((uint16_t)gas->address, (uint8_t)value);
        else if (gas->bit_width <= 16) port_outw((uint16_t)gas->address, (uint16_t)value);
        else port_outl((uint16_t)gas->address, value);
    } else {
        volatile uint8_t* address = (volatile uint8_t*)(uintptr_t)gas->address;
        if (gas->bit_width <= 8) *(volatile uint8_t*)address = (uint8_t)value;
        else if (gas->bit_width <= 16) *(volatile uint16_t*)address = (uint16_t)value;
        else *(volatile uint32_t*)address = value;
    }
    return true;
}

static acpi_gas_t acpi_gas_at_offset(acpi_gas_t gas, uint64_t offset,
                                     uint8_t width) {
    gas.address += offset;
    gas.bit_width = width;
    gas.bit_offset = 0;
    gas.access_size = width <= 8 ? 1 : width <= 16 ? 2 : 3;
    return gas;
}

static bool acpi_aml_read_integer(const uint8_t** cursor, const uint8_t* end,
                                 uint64_t* value) {
    if (!cursor || !*cursor || *cursor >= end || !value) return false;
    uint8_t opcode = *(*cursor)++;
    switch (opcode) {
        case 0x00: *value = 0; return true;
        case 0x01: *value = 1; return true;
        case 0x0A:
            if (end - *cursor < 1) return false;
            *value = *(*cursor)++;
            return true;
        case 0x0B:
            if (end - *cursor < 2) return false;
            *value = acpi_read_u16(*cursor);
            *cursor += 2;
            return true;
        case 0x0C:
            if (end - *cursor < 4) return false;
            *value = acpi_read_u32(*cursor);
            *cursor += 4;
            return true;
        case 0x0E:
            if (end - *cursor < 8) return false;
            *value = acpi_read_u64(*cursor);
            *cursor += 8;
            return true;
        default:
            return false;
    }
}

static bool acpi_find_s5(const acpi_sdt_header_t* dsdt,
                         uint8_t* sleep_type_a, uint8_t* sleep_type_b) {
    if (!dsdt || dsdt->length < sizeof(*dsdt) ||
        dsdt->length > 16 * 1024 * 1024 ||
        strncmp(dsdt->signature, "DSDT", 4) != 0) return false;

    const uint8_t* table = (const uint8_t*)dsdt;
    const uint8_t* end = table + dsdt->length;
    for (const uint8_t* p = table + sizeof(*dsdt); end - p >= 8; p++) {
        if (p[0] != 0x08 || p[1] != '_' || p[2] != 'S' ||
            p[3] != '5' || p[4] != '_') continue;
        const uint8_t* package = p + 5;
        if (package >= end || *package++ != 0x12 || package >= end) continue;

        const uint8_t* length_start = package;
        uint8_t lead = *package++;
        uint8_t follow_bytes = lead >> 6;
        if ((size_t)(end - package) < follow_bytes) continue;
        uint32_t package_length = lead & (follow_bytes ? 0x0F : 0x3F);
        for (uint8_t i = 0; i < follow_bytes; i++) {
            package_length |= (uint32_t)package[i] << (4 + 8 * i);
        }
        package += follow_bytes;
        if (package_length < (uint32_t)(package - length_start) ||
            package_length > (uint32_t)(end - length_start)) continue;
        const uint8_t* package_end = length_start + package_length;
        if (package >= package_end || *package++ < 2) continue;

        uint64_t type_a, type_b;
        if (acpi_aml_read_integer(&package, package_end, &type_a) &&
            acpi_aml_read_integer(&package, package_end, &type_b)) {
            *sleep_type_a = (uint8_t)(type_a & 7);
            *sleep_type_b = (uint8_t)(type_b & 7);
            return true;
        }
    }
    return false;
}

static void acpi_init_power(const acpi_sdt_header_t* fadt_header) {
    if (!fadt_header || fadt_header->length < 90 ||
        strncmp(fadt_header->signature, "FACP", 4) != 0) return;
    const uint8_t* fadt = (const uint8_t*)fadt_header;
    uint8_t pm1_event_length = fadt[88];
    uint8_t pm1_control_length = fadt[89];
    uint32_t pm1a_event_legacy = acpi_read_u32(fadt + 56);
    uint32_t pm1b_event_legacy = acpi_read_u32(fadt + 60);
    uint32_t pm1a_legacy = acpi_read_u32(fadt + 64);
    uint32_t pm1b_legacy = acpi_read_u32(fadt + 68);

    if (fadt_header->length >= 160) {
        acpi_gas_t extended = acpi_read_gas(fadt + 148);
        if (acpi_gas_valid(&extended, 32)) {
            g_pm1a_event = extended;
            g_pm1a_event_available = true;
        }
    }
    if (!g_pm1a_event_available && pm1a_event_legacy && pm1_event_length >= 4) {
        g_pm1a_event = (acpi_gas_t){1, 32, 0, 3, pm1a_event_legacy};
        g_pm1a_event_available = acpi_gas_valid(&g_pm1a_event, 32);
    }

    if (fadt_header->length >= 172) {
        acpi_gas_t extended = acpi_read_gas(fadt + 160);
        if (acpi_gas_valid(&extended, 32)) {
            g_pm1b_event = extended;
            g_pm1b_event_available = true;
        }
    }
    if (!g_pm1b_event_available && pm1b_event_legacy && pm1_event_length >= 4) {
        g_pm1b_event = (acpi_gas_t){1, 32, 0, 3, pm1b_event_legacy};
        g_pm1b_event_available = acpi_gas_valid(&g_pm1b_event, 32);
    }

    if (fadt_header->length >= 53) {
        g_smi_command_port = (uint16_t)acpi_read_u32(fadt + 48);
        g_acpi_enable_value = fadt[52];
    }

    if (fadt_header->length >= 184) {
        acpi_gas_t extended = acpi_read_gas(fadt + 172);
        if (acpi_gas_valid(&extended, 16)) {
            g_pm1a_control = extended;
            g_pm1a_available = true;
        }
    }
    if (!g_pm1a_available && pm1a_legacy && pm1_control_length >= 2) {
        g_pm1a_control = (acpi_gas_t){1, 16, 0, 2, pm1a_legacy};
        g_pm1a_available = acpi_gas_valid(&g_pm1a_control, 16);
    }

    if (fadt_header->length >= 196) {
        acpi_gas_t extended = acpi_read_gas(fadt + 184);
        if (acpi_gas_valid(&extended, 16)) {
            g_pm1b_control = extended;
            g_pm1b_available = true;
        }
    }
    if (!g_pm1b_available && pm1b_legacy && pm1_control_length >= 2) {
        g_pm1b_control = (acpi_gas_t){1, 16, 0, 2, pm1b_legacy};
        g_pm1b_available = acpi_gas_valid(&g_pm1b_control, 16);
    }

    if (fadt_header->length >= 129 &&
        (acpi_read_u32(fadt + 112) & (1U << 10))) {
        g_reset_register = acpi_read_gas(fadt + 116);
        g_reset_value = fadt[128];
        g_reset_available = acpi_gas_valid(&g_reset_register, 8);
    }

    uint64_t dsdt_address = acpi_read_u32(fadt + 40);
    if (fadt_header->length >= 148) {
        uint64_t extended_dsdt = acpi_read_u64(fadt + 140);
        if (extended_dsdt) dsdt_address = extended_dsdt;
    }
    if (dsdt_address && dsdt_address <= UINTPTR_MAX) {
        g_s5_available = acpi_find_s5((const acpi_sdt_header_t*)(uintptr_t)dsdt_address,
                                      &g_s5_type_a, &g_s5_type_b);
    }

    serial_write_str("ACPI: power controls PM1a=");
    serial_write_str(g_pm1a_available ? "yes" : "no");
    serial_write_str(" S5=");
    serial_write_str(g_s5_available ? "yes" : "no");
    serial_write_str(" reset=");
    serial_write_str(g_reset_available ? "yes" : "no");
    serial_write_str("\n");

    g_power_button_available = g_pm1a_event_available && g_pm1a_available;
    if (!g_power_button_available) {
        serial_write_str("ACPI: power-button event registers unavailable\n");
        return;
    }

    uint32_t control;
    if (!acpi_gas_read(&g_pm1a_control, &control)) {
        g_power_button_available = false;
        return;
    }
    if (!(control & 1U) && g_smi_command_port && g_acpi_enable_value) {
        port_outb(g_smi_command_port, g_acpi_enable_value);
        uint32_t timeout = 1000000;
        while (timeout-- && !(control & 1U)) {
            if (!acpi_gas_read(&g_pm1a_control, &control)) break;
            __asm__ volatile("pause");
        }
    }
    if (!(control & 1U)) {
        serial_write_str("ACPI: firmware did not enable SCI mode\n");
        g_power_button_available = false;
        return;
    }

    acpi_gas_t status_a = acpi_gas_at_offset(g_pm1a_event, 0, 16);
    acpi_gas_t enable_a = acpi_gas_at_offset(g_pm1a_event, 2, 16);
    uint32_t enables;
    if (acpi_gas_read(&status_a, &control) && (control & 0x0100)) {
        acpi_gas_write(&status_a, 0x0100);
    }
    if (!acpi_gas_read(&enable_a, &enables) ||
        !acpi_gas_write(&enable_a, enables | 0x0100)) {
        g_power_button_available = false;
        return;
    }

    if (g_pm1b_event_available) {
        acpi_gas_t status_b = acpi_gas_at_offset(g_pm1b_event, 0, 16);
        acpi_gas_t enable_b = acpi_gas_at_offset(g_pm1b_event, 2, 16);
        if (acpi_gas_read(&status_b, &control) && (control & 0x0100)) {
            acpi_gas_write(&status_b, 0x0100);
        }
        if (acpi_gas_read(&enable_b, &enables)) {
            acpi_gas_write(&enable_b, enables | 0x0100);
        }
    }
    serial_write_str("ACPI: power-button event enabled\n");
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

static const acpi_rsdp_t* find_legacy_rsdp(void) {
    uint32_t ebda_segment = *(volatile uint16_t*)(uintptr_t)0x40E;
    uintptr_t ebda = (uintptr_t)ebda_segment << 4;

    for (uintptr_t address = ebda; address < ebda + 0x400; address += 16) {
        const acpi_rsdp_t* candidate = (const acpi_rsdp_t*)address;
        if (strncmp(candidate->signature, "RSD PTR ", 8) == 0 &&
            acpi_checksum(candidate, 20) == 0) {
            return candidate;
        }
    }

    for (uintptr_t address = 0xE0000; address < 0x100000; address += 16) {
        const acpi_rsdp_t* candidate = (const acpi_rsdp_t*)address;
        if (strncmp(candidate->signature, "RSD PTR ", 8) == 0 &&
            acpi_checksum(candidate, 20) == 0) {
            return candidate;
        }
    }

    return NULL;
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
        serial_write_str("ACPI: No RSDP tag; scanning BIOS ACPI areas\n");
        rsdp = find_legacy_rsdp();
        if (!rsdp) {
            serial_write_str("ACPI: RSDP not found\n");
            return false;
        }
    }

    if (acpi_checksum(rsdp, 20) != 0) {
        serial_write_str("ACPI: RSDP checksum failed\n");
        return false;
    }
    if (rsdp->revision >= 2 && acpi_checksum(rsdp, rsdp->length) != 0) {
        serial_write_str("ACPI: Extended RSDP checksum failed\n");
        return false;
    }

    const acpi_sdt_header_t* fadt_hdr = find_table(rsdp, "FACP");
    if (fadt_hdr) {
        acpi_init_power(fadt_hdr);
    } else {
        serial_write_str("ACPI: FADT not found; power control unavailable\n");
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

static uint64_t acpi_disable_interrupts(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void acpi_restore_interrupts(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static void acpi_reset_delay(void) {
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        __asm__ volatile("pause");
    }
}

bool acpi_shutdown(void) {
    if (!g_pm1a_available || !g_s5_available) {
        serial_write_str("ACPI: S5 shutdown is unavailable\n");
        return false;
    }

    uint64_t flags = acpi_disable_interrupts();
    uint32_t pm1a_value;
    uint32_t pm1b_value = 0;
    if (!acpi_gas_read(&g_pm1a_control, &pm1a_value) ||
        (g_pm1b_available && !acpi_gas_read(&g_pm1b_control, &pm1b_value))) {
        acpi_restore_interrupts(flags);
        serial_write_str("ACPI: could not read PM1 control register\n");
        return false;
    }

    const uint32_t sleep_mask = (7U << 10) | (1U << 13);
    pm1a_value = (pm1a_value & ~sleep_mask) |
                 ((uint32_t)g_s5_type_a << 10) | (1U << 13);
    if (!acpi_gas_write(&g_pm1a_control, pm1a_value)) {
        acpi_restore_interrupts(flags);
        serial_write_str("ACPI: could not write PM1a shutdown control\n");
        return false;
    }

    if (g_pm1b_available) {
        pm1b_value = (pm1b_value & ~sleep_mask) |
                     ((uint32_t)g_s5_type_b << 10) | (1U << 13);
        if (!acpi_gas_write(&g_pm1b_control, pm1b_value)) {
            acpi_restore_interrupts(flags);
            serial_write_str("ACPI: could not write PM1b shutdown control\n");
            return false;
        }
    }

    acpi_reset_delay();
    acpi_restore_interrupts(flags);
    serial_write_str("ACPI: shutdown request did not power off the machine\n");
    return false;
}

bool acpi_poll_power_button(void) {
    if (!g_power_button_available) return false;

    acpi_gas_t status_a = acpi_gas_at_offset(g_pm1a_event, 0, 16);
    uint32_t status;
    if (acpi_gas_read(&status_a, &status) && (status & 0x0100)) {
        acpi_gas_write(&status_a, 0x0100);
        return true;
    }

    if (g_pm1b_event_available) {
        acpi_gas_t status_b = acpi_gas_at_offset(g_pm1b_event, 0, 16);
        if (acpi_gas_read(&status_b, &status) && (status & 0x0100)) {
            acpi_gas_write(&status_b, 0x0100);
            return true;
        }
    }
    return false;
}

bool acpi_reboot(void) {
    uint64_t flags = acpi_disable_interrupts();
    if (g_reset_available && acpi_gas_write(&g_reset_register, g_reset_value)) {
        acpi_reset_delay();
    }

    uint32_t timeout = 1000000;
    while ((port_inb(0x64) & 0x02) && timeout--) {
        __asm__ volatile("pause");
    }
    if (timeout) {
        port_outb(0x64, 0xFE);
        acpi_reset_delay();
    }

    port_outb(0xCF9, 0x06);
    acpi_reset_delay();
    acpi_restore_interrupts(flags);
    serial_write_str("ACPI: reset request failed\n");
    return false;
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