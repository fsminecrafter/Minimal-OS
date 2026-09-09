#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

bool ioapic_init(uintptr_t phys_addr);
void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint32_t target_apic_id);
void ioapic_mask_irq(uint8_t gsi);

#endif // IOAPIC_H