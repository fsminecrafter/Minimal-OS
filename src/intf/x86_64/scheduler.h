// src/intf/x86_64/scheduler.h
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

// ===========================================
// MLFQ-lite scheduling levels
// ===========================================
//
// Each online core owns its own ready queue (see scheduler.c), split
// into these levels. Level 0 is always dispatched before level 1,
// which is always dispatched before level 2 - interactive work
// (terminal input, keyboard handling) preempts long-running
// background work (audio decode loops, etc) without needing a full
// CFS-style red-black tree.
//
// A process that burns through its whole quantum without blocking is
// demoted one level (it looks CPU-bound). A process that blocks
// (sleep(), unpause after a wait) is promoted back to level 0 on
// wake - blocking is the classic signal of I/O-bound / interactive
// work. A periodic full boost (SCHED_BOOST_INTERVAL_TICKS in
// scheduler.c) prevents anything from starving permanently at the
// bottom level.
#define SCHED_NUM_LEVELS         3
#define SCHED_LEVEL_INTERACTIVE  0
#define SCHED_LEVEL_NORMAL       1
#define SCHED_LEVEL_BACKGROUND   2

// Quantum, in scheduler ticks, a process may hold its level before
// being demoted. Indexed by SCHED_LEVEL_*.
extern const uint32_t sched_level_quantum_ticks[SCHED_NUM_LEVELS];

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

// ===========================================
// Per-CPU run queue API
// ===========================================

// Places a newly created process onto the least-loaded online core's
// run queue, at the interactive level, and records that assignment in
// proc->sched_cpu. Called once by proc_create().
void scheduler_enqueue_new(process_t* proc);

// Re-enqueues a process that was previously removed from its run
// queue via scheduler_dequeue() (e.g. unpauseProcess()). Keeps it on
// the same core it was already assigned to rather than picking a new
// one, and resets it to the interactive level - same treatment as
// waking from sleep.
void scheduler_enqueue_existing(process_t* proc);

// Removes `proc` from whichever per-CPU run queue it's currently
// sitting in, if any. Safe to call on a process that isn't queued
// (currently running, already removed, never queued) - a no-op in
// that case. MUST be called when marking a process ZOMBIE or PAUSED,
// so it can never be dispatched again and the global zombie-cleanup
// pass in schedule() can safely free it once it stops being anyone's
// current_process.
void scheduler_dequeue(process_t* proc);

// Moves one process from the busiest online core's run queue to the
// idlest one's, if the imbalance is large enough to be worth it.
// Called periodically from scheduler_tick() - see
// SCHED_REBALANCE_INTERVAL_TICKS in scheduler.c.
void scheduler_rebalance(void);

// Promotes every process on every core's run queue back to the
// interactive level. Called periodically from scheduler_tick() - see
// SCHED_BOOST_INTERVAL_TICKS in scheduler.c.
void scheduler_priority_boost(void);

#endif // SCHEDULER_H