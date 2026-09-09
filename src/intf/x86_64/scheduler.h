#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "x86_64/proc.h"
#include "x86_64/smp.h"

// Current process (per-CPU).
//
// This used to be a single global `process_t* current_process`. With
// SMP, every core needs its OWN "process running right now" pointer,
// so it now lives in that core's g_cpus[] slot (see smp.h). This macro
// keeps every existing read, write, and dereference of
// `current_process` elsewhere in the kernel working completely
// unmodified: it expands to `(*current_process_slot())`, a valid
// lvalue exactly like a plain global variable was.
static inline process_t** current_process_slot(void) {
    return &g_cpus[smp_current_cpu_id()].current_process;
}
#define current_process (*current_process_slot())

// Scheduling functions
void schedule(void);
void scheduler_tick(void);
void ready(void);
void schedulerInit();
// Exits current process.
void process_exit(void) __attribute__((__noreturn__));

// Sleep/wake functions
void sleep(uint64_t milliseconds);

// Statistics
void scheduler_get_stats(uint64_t* switches, uint64_t* idle);
void scheduler_print_stats(void);

// Debug
void currentstate(void);

// Global scheduler-list lock (real spinlock, not just cli). Used by
// schedule() and proc_create() to protect proc_list_head; exposed so
// any future code that also walks that list can join the same lock
// instead of inventing its own.
uint64_t scheduler_lock(void);
void scheduler_unlock(uint64_t flags);

#endif // SCHEDULER_H