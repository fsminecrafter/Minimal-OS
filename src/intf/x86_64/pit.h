#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency);
uint64_t pit_get_ticks();
void pit_irq_handler();
void setup_kernel_interrupts();

#endif
