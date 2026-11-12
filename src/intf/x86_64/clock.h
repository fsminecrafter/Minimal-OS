#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

// Call once on the boot CPU after the PIT runs at 1 kHz and interrupts are on.
void clock_init(void);

// Nanoseconds since boot. Never goes backwards, callable from any CPU/context.
uint64_t clock_monotonic_ns(void);

// Nanoseconds since 1970-01-01 by the system (RTC-backed) clock.
uint64_t clock_realtime_ns(void);

// Calibrated TSC frequency in Hz, only when the TSC is invariant; else 0.
uint64_t clock_tsc_hz(void);

#endif
