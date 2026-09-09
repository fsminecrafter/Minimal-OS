#include "x86_64/lapic_timer.h"
#include "x86_64/lapic.h"
#include "x86_64/acpi.h"
#include "x86_64/mmio.h"
#include "x86_64/scheduler.h"
#include "serial.h"
#include <stddef.h>

/*
 * This maps its own alias of the LAPIC MMIO page rather than reaching
 * into lapic.c's private mapping. That's safe and cheap: xAPIC MMIO is
 * banked per-core in hardware - every core reads/writes to the exact
 * same physical address (from acpi_get_lapic_phys_addr()) but always
 * hits its OWN local APIC. Two virtual aliases of that one physical
 * page from two different core's timer-init calls don't conflict.
 *
 * The mapping itself (mmio_alloc_va() + map_mmio_page()) is done at
 * most once, guarded by double-checked locking, because mmio_alloc_va()
 * mutates a single un-locked global VA cursor and is not safe to call
 * concurrently from two cores. ap_entry_c() (smp.c) calls
 * lapic_timer_init() BEFORE marking its core online, and
 * smp_start_aps() waits for "online" before sending the next AP's
 * SIPI - so in practice only one core is ever inside this function at
 * a time anyway; the lock is a correctness backstop, not load-bearing.
 */
static volatile uint32_t* g_lapic_timer_mmio = NULL;
static volatile int       g_lapic_timer_map_lock = 0;

static inline uint32_t lt_read(uint32_t reg) {
    return g_lapic_timer_mmio[reg / 4];
}
static inline void lt_write(uint32_t reg, uint32_t value) {
    g_lapic_timer_mmio[reg / 4] = value;
}

static bool lapic_timer_ensure_mapped(void) {
    if (g_lapic_timer_mmio) return true;

    while (__sync_lock_test_and_set(&g_lapic_timer_map_lock, 1)) {
        asm volatile("pause" ::: "memory");
    }

    if (!g_lapic_timer_mmio) {
        uintptr_t phys = acpi_get_lapic_phys_addr();
        void* va = mmio_alloc_va(0x1000);
        if (va && map_mmio_page((uintptr_t)va, phys, 0x1000)) {
            g_lapic_timer_mmio = (volatile uint32_t*)va;
        } else {
            serial_write_str("LAPIC timer: failed to map MMIO region\n");
        }
    }

    __sync_lock_release(&g_lapic_timer_map_lock);
    return g_lapic_timer_mmio != NULL;
}

bool lapic_timer_init(uint32_t vector, uint32_t initial_count, uint8_t divide_value) {
    if (!lapic_timer_ensure_mapped()) return false;

    lt_write(LAPIC_REG_TIMER_DIV, divide_value & 0xF);
    // Bit 17 = periodic mode; low byte = vector.
    lt_write(LAPIC_REG_LVT_TIMER, (vector & 0xFF) | (1u << 17));
    lt_write(LAPIC_REG_TIMER_INIT, initial_count);

    serial_write_str("LAPIC timer: started, vector=0x");
    serial_write_hex(vector);
    serial_write_str("\n");
    return true;
}

void lapic_timer_eoi(void) {
    if (!g_lapic_timer_mmio) return;
    lt_write(LAPIC_REG_EOI, 0);
}

/*
 * Fires periodically on whichever core owns this timer. Deliberately
 * calls ONLY scheduler_tick() - not time_tick()/audio_update()/usb_poll(),
 * which stay BSP-only side effects of the PIT tick in pit_irq_handler()
 * (pit.c). scheduler_tick() -> schedule() already goes through the
 * real cross-core scheduler_lock() spinlock (scheduler.c), so this is
 * safe to call concurrently from multiple cores.
 */
void lapic_timer_irq_handler(void) {
    scheduler_tick();
    lapic_timer_eoi();
}