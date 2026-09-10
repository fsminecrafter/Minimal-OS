#pragma once
#include <stdint.h>
#include <stdbool.h>

// Vector used for the per-core LAPIC periodic timer interrupt.
// Must not collide with the PIC's remapped range (0x20-0x2F) or the
// LAPIC spurious vector (0xFF, see LAPIC_SPURIOUS_VECTOR in lapic.h).
#define LAPIC_TIMER_VECTOR 0x32

// Configures and starts this core's LAPIC timer in periodic mode at
// the given vector, initial count, and divide value (SDM 3-bit
// encoding: 0=/2 1=/4 2=/8 3=/16 8=/32 9=/64 10=/128 11=/1).
// Must be called after lapic_init() has run on this core.
//
// This is the raw, uncalibrated primitive: `initial_count` is used
// exactly as given, with no relation to real time. Most callers want
// lapic_timer_init_calibrated() instead, which computes an
// appropriate initial_count for a target frequency. This function
// stays exposed directly for callers that already know the correct
// initial_count (including lapic_timer_init_calibrated() itself,
// internally) or that deliberately want an uncalibrated one-off count.
bool lapic_timer_init(uint32_t vector, uint32_t initial_count, uint8_t divide_value);

// Calibrates this core's LAPIC timer against the wall clock
// (time_get_uptime_ms(), driven by the master core's PIT interrupt -
// see time_tick() in time.c) and then starts it in periodic mode at
// approximately `target_hz`. Must be called after lapic_init() has
// run on this core, and only once the PIT is already ticking (true
// by the time smp_start_aps() brings up any AP - see ap_entry_c()).
//
// Falls back to a fixed placeholder initial count (see lapic_timer.c)
// if calibration fails - e.g. the wall clock didn't advance during
// the calibration window because this was called too early, or
// because the core driving time_tick() has its interrupts wedged.
// Never returns false for a calibration failure on its own - a
// scheduler relying on this timer firing at all is better served by
// *a* period than by an unprogrammed timer; only a genuine MMIO
// mapping failure (see lapic_timer_init()) returns false.
bool lapic_timer_init_calibrated(uint32_t vector, uint32_t target_hz, uint8_t divide_value);

// Sends EOI for the LAPIC timer interrupt. Call from the ISR handler.
void lapic_timer_eoi(void);