#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "x86_64/proc.h"

// Current process
extern process_t* current_process;

// Scheduling functions
void schedule(void);
void scheduler_tick(void);
void ready(void);

// Sleep/wake functions
void sleep(uint64_t milliseconds);

// Statistics
void scheduler_get_stats(uint64_t* switches, uint64_t* idle);
void scheduler_print_stats(void);

// Debug
void currentstate(void);

#endif // SCHEDULER_H