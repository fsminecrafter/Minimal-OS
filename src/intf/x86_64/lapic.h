#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_VERSION    0x030
#define LAPIC_REG_TPR        0x080
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SVR        0x0F0
#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define LAPIC_ICR_DELIVERY_INIT    (5 << 8)
#define LAPIC_ICR_DELIVERY_STARTUP (6 << 8)
#define LAPIC_ICR_LEVEL_ASSERT     (1 << 14)
#define LAPIC_ICR_DEST_PHYSICAL    (0 << 11)

// Vector the spurious-interrupt handler would run at. NOTE: idt.c's
// idt[256] table only has 3 real entries installed (double fault,
// IRQ0, IRQ1) - a spurious interrupt landing on this vector today
// hits a zeroed/absent IDT gate. Harmless unless one actually fires;
// flagged here rather than silently papered over.
#define LAPIC_SPURIOUS_VECTOR 0xFF

// Maps the LAPIC MMIO region and enables it, via both the
// IA32_APIC_BASE MSR and the SVR software-enable bit. Call once per
// core - the MMIO mapping itself only happens once (page tables are
// shared across cores), but the MSR/SVR enable is real per-core
// hardware state and must be redone by every AP.
bool lapic_init(void);

uint32_t lapic_get_id(void);
void     lapic_send_eoi(void);

void lapic_send_init_ipi(uint32_t target_apic_id);
void lapic_send_startup_ipi(uint32_t target_apic_id, uint8_t vector);
void lapic_wait_icr_idle(void);

#endif // LAPIC_H