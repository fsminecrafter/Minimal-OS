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
// NOT calibrated against a real time reference - see the comment in
// ap_entry_c() (smp.c) for why that's an acceptable starting point.
bool lapic_timer_init(uint32_t vector, uint32_t initial_count, uint8_t divide_value);

// Sends EOI for the LAPIC timer interrupt. Call from the ISR handler.
void lapic_timer_eoi(void);