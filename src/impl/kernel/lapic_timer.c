#include "x86_64/lapic_timer.h"
#include "x86_64/lapic.h"
#include "x86_64/acpi.h"
#include "x86_64/mmio.h"
#include "x86_64/scheduler.h"
#include "serial.h"
#include "time.h"
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
 * lapic_timer_init_calibrated() BEFORE marking its core online, and
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
    // Bit 17 = periodic mode; low byte = vector. Writing this also
    // clears the mask bit (16), so any prior calibration mask is lifted
    // here.
    lt_write(LAPIC_REG_LVT_TIMER, (vector & 0xFF) | (1u << 17));
    lt_write(LAPIC_REG_TIMER_INIT, initial_count);

    serial_write_str("LAPIC timer: started, vector=0x");
    serial_write_hex(vector);
    serial_write_str(", initial_count=");
    serial_write_dec(initial_count);
    serial_write_str("\n");
    return true;
}

// ===========================================
// CALIBRATION
// ===========================================

// How long to let the timer free-run while measuring it against the
// wall clock. Short enough to add negligible AP startup latency (this
// runs once per AP, during smp_start_aps()'s already-bounded bring-up
// window), long enough to keep quantization error in the ticks/ms
// figure small - at a few hundred MHz post-divide, even 10ms is
// several hundred thousand ticks, so +/-1ms of PIT-tick granularity in
// the wall-clock read is well under 1% error.
#define LAPIC_CALIBRATION_WINDOW_MS 10

// Measures how many LAPIC timer ticks (at the given divide setting)
// occur per millisecond of real time, using the PIT-driven wall clock
// (time_get_uptime_ms()) as the reference. Must be called with the
// LAPIC already mapped (caller's responsibility - see
// lapic_timer_init_calibrated()).
//
// Masks the timer's LVT entry for the duration so it can never raise
// an interrupt while being used as a free-running stopwatch, and
// leaves the timer stopped (INIT=0) on return - the caller is
// responsible for programming the real periodic setup afterwards via
// lapic_timer_init().
//
// Returns 0 if the wall clock never advanced during the calibration
// window - e.g. called before the PIT is ticking, or the core that
// owns time_tick() has interrupts wedged. Callers MUST treat 0 as
// "calibration failed" rather than dividing by it.
static uint32_t lapic_calibrate_ticks_per_ms(uint8_t divide_value) {
    lt_write(LAPIC_REG_TIMER_DIV, divide_value & 0xF);

    // Mask (bit 16); one-shot mode (bit 17 = 0) since we never want it
    // to reload. Vector value is irrelevant while masked.
    lt_write(LAPIC_REG_LVT_TIMER, (1u << 16));

    const uint32_t CALIB_START_COUNT = 0xFFFFFFFFu;
    lt_write(LAPIC_REG_TIMER_INIT, CALIB_START_COUNT);

    uint64_t start_ms    = time_get_uptime_ms();
    uint64_t deadline_ms  = start_ms + LAPIC_CALIBRATION_WINDOW_MS;

    // Bounded busy-wait - same pattern as every other calibrated-delay
    // loop in this kernel (see ahci_sleep_ms()/minimafs_sleep_ms() in
    // ahci.c/minimafs.c): never trust the wall clock to advance without
    // an iteration cap, in case this core's own IF state or a stalled
    // PIT ever leaves it standing still.
    const uint32_t MAX_ITER = 200000000u;
    uint32_t iter = 0;
    while (time_get_uptime_ms() < deadline_ms) {
        asm volatile("pause" ::: "memory");
        if (++iter > MAX_ITER) break;
    }

    uint64_t elapsed_ms = time_get_uptime_ms() - start_ms;
    uint32_t current     = lt_read(LAPIC_REG_TIMER_CUR);
    uint32_t elapsed_ticks = CALIB_START_COUNT - current;

    // Stop the timer - this one-shot countdown must not be left
    // running into the real periodic setup that follows.
    lt_write(LAPIC_REG_TIMER_INIT, 0);

    if (elapsed_ms == 0) {
        return 0;  // wall clock never moved - calibration failed
    }

    return (uint32_t)(elapsed_ticks / elapsed_ms);
}

bool lapic_timer_init_calibrated(uint32_t vector, uint32_t target_hz, uint8_t divide_value) {
    if (!lapic_timer_ensure_mapped()) return false;
    if (target_hz == 0) target_hz = 100;

    uint32_t ticks_per_ms = lapic_calibrate_ticks_per_ms(divide_value);

    uint32_t initial_count;
    if (ticks_per_ms == 0) {
        // Calibration failed - fall back to the old fixed placeholder
        // rather than dividing by zero or leaving the timer
        // unprogrammed. This keeps the scheduler tick alive even in
        // the degenerate case, just without the fairness guarantee
        // calibration is meant to provide.
        serial_write_str("LAPIC timer: calibration failed (wall clock did not advance), "
                          "using fallback initial_count\n");
        initial_count = 0x100000;
    } else {
        uint64_t computed = ((uint64_t)ticks_per_ms * 1000ULL) / target_hz;
        if (computed == 0) computed = 1;
        if (computed > 0xFFFFFFFFULL) computed = 0xFFFFFFFFULL;
        initial_count = (uint32_t)computed;

        serial_write_str("LAPIC timer: calibrated ");
        serial_write_dec(ticks_per_ms);
        serial_write_str(" ticks/ms @ divide=");
        serial_write_dec(divide_value);
        serial_write_str(", target ");
        serial_write_dec(target_hz);
        serial_write_str(" Hz\n");
    }

    return lapic_timer_init(vector, initial_count, divide_value);
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