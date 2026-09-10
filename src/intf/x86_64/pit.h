#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency);
uint64_t pit_get_ticks();
void pit_irq_handler();
void setup_kernel_interrupts();

// Returns the frequency (Hz) the PIT was configured for via pit_init().
// Used by ap_entry_c() (smp.c) so every core's calibrated LAPIC timer
// targets the same tick rate as the BSP's PIT-driven tick, keeping
// MLFQ quantum lengths comparable in wall-clock terms across cores.
uint32_t pit_get_frequency(void);

#endif