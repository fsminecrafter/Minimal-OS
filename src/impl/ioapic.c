#include "x86_64/ioapic.h"
#include "x86_64/mmio.h"
#include "serial.h"

#define IOAPIC_REG_VER    0x01
#define IOAPIC_REG_REDTBL 0x10

static volatile uint32_t* g_ioapic_va = NULL;

static uint32_t ioapic_read(uint32_t reg) {
    g_ioapic_va[0] = reg;
    return g_ioapic_va[4];
}
static void ioapic_write(uint32_t reg, uint32_t value) {
    g_ioapic_va[0] = reg;
    g_ioapic_va[4] = value;
}

bool ioapic_init(uintptr_t phys_addr) {
    void* va = mmio_alloc_va(0x1000);
    if (!va || !map_mmio_page((uintptr_t)va, phys_addr, 0x1000)) {
        serial_write_str("IOAPIC: failed to map MMIO region\n");
        return false;
    }
    g_ioapic_va = (volatile uint32_t*)va;

    uint32_t max_entries = ((ioapic_read(IOAPIC_REG_VER) >> 16) & 0xFF) + 1;
    serial_write_str("IOAPIC: mapped, ");
    serial_write_dec(max_entries);
    serial_write_str(" redirection entries\n");

    for (uint32_t i = 0; i < max_entries; i++) {
        ioapic_write(IOAPIC_REG_REDTBL + i * 2, (1 << 16)); // masked
        ioapic_write(IOAPIC_REG_REDTBL + i * 2 + 1, 0);
    }
    return true;
}

void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint32_t target_apic_id) {
    if (!g_ioapic_va) return;
    ioapic_write(IOAPIC_REG_REDTBL + gsi * 2 + 1, target_apic_id << 24);
    ioapic_write(IOAPIC_REG_REDTBL + gsi * 2, vector); // fixed, edge, active-high, unmasked
}

void ioapic_mask_irq(uint8_t gsi) {
    if (!g_ioapic_va) return;
    uint32_t low = ioapic_read(IOAPIC_REG_REDTBL + gsi * 2);
    ioapic_write(IOAPIC_REG_REDTBL + gsi * 2, low | (1 << 16));
}