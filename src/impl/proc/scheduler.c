// src/impl/proc/scheduler.c
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "panic.h"
#include "print.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "x86_64/safeints.h"
#include "x86_64/spinlock.h"

#define SCHED_DEBUG 0   // 0 = off, 1 = important, 2 = verbose

bool scheduler_on = false;

// Linked list of processes
extern process_t* proc_list_head;

// Forward declaration of context switch assembly routine
extern void context_switch(process_t* current, process_t* next);

// Statistics
static uint64_t total_context_switches = 0;
static uint64_t idle_cycles = 0;

// Real cross-core lock guarding proc_list_head + current_process
// transitions. See spinlock.h for why cli() alone isn't enough once a
// second physical core exists. With per-CPU run queues this is now
// ONLY needed for the global creation/cleanup list - the hot dispatch
// path below never touches it.
static spinlock_t g_sched_lock = SPINLOCK_INIT;

uint64_t scheduler_lock(void) {
    return spinlock_acquire(&g_sched_lock);
}

void scheduler_unlock(uint64_t flags) {
    spinlock_release(&g_sched_lock, flags);
}

// ===========================================
// PER-CPU RUN QUEUES
// ===========================================

typedef struct {
    process_t* head[SCHED_NUM_LEVELS];
    process_t* tail[SCHED_NUM_LEVELS];
    uint32_t   count[SCHED_NUM_LEVELS];
    spinlock_t lock;
} percpu_runqueue_t;

// BSS-zeroed, so every queue starts empty and every lock starts
// unlocked without needing an explicit init pass.
static percpu_runqueue_t g_runqueues[MAX_CPUS];

const uint32_t sched_level_quantum_ticks[SCHED_NUM_LEVELS] = {
    2,   // interactive: short quantum, dispatched first
    5,   // normal
    10,  // background: long quantum, only runs once higher levels are empty
};

// How often (in scheduler ticks) to run the anti-starvation boost and
// the cross-core rebalance pass. Gated on a single shared counter (see
// g_global_tick_count below) so these fire exactly once per interval
// total, not once per interval PER CORE - every online core's LAPIC
// timer calls scheduler_tick() independently.
#define SCHED_BOOST_INTERVAL_TICKS      2000
#define SCHED_REBALANCE_INTERVAL_TICKS  20

static void rq_push(percpu_runqueue_t* rq, process_t* proc, uint32_t level) {
    proc->rq_next = NULL;
    if (rq->tail[level]) {
        rq->tail[level]->rq_next = proc;
    } else {
        rq->head[level] = proc;
    }
    rq->tail[level] = proc;
    rq->count[level]++;
}

static process_t* rq_pop(percpu_runqueue_t* rq, uint32_t level) {
    process_t* proc = rq->head[level];
    if (!proc) return NULL;
    rq->head[level] = proc->rq_next;
    if (!rq->head[level]) rq->tail[level] = NULL;
    proc->rq_next = NULL;
    rq->count[level]--;
    return proc;
}

static process_t* rq_pop_highest(percpu_runqueue_t* rq) {
    for (uint32_t level = 0; level < SCHED_NUM_LEVELS; level++) {
        process_t* proc = rq_pop(rq, level);
        if (proc) return proc;
    }
    return NULL;
}

static uint32_t rq_total(percpu_runqueue_t* rq) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < SCHED_NUM_LEVELS; i++) total += rq->count[i];
    return total;
}

static uint32_t pick_least_loaded_cpu(void) {
    uint32_t online = smp_online_cpu_count();
    if (online == 0) online = 1;
    if (online > MAX_CPUS) online = MAX_CPUS;

    uint32_t best_cpu = 0;
    uint32_t best_load = 0xFFFFFFFFu;

    for (uint32_t cpu = 0; cpu < online; cpu++) {
        uint64_t flags = spinlock_acquire(&g_runqueues[cpu].lock);
        uint32_t load = rq_total(&g_runqueues[cpu]);
        spinlock_release(&g_runqueues[cpu].lock, flags);

        if (load < best_load) {
            best_load = load;
            best_cpu = cpu;
        }
    }
    return best_cpu;
}

void scheduler_enqueue_new(process_t* proc) {
    if (!proc) return;

    proc->sched_level = SCHED_LEVEL_INTERACTIVE;
    proc->sched_ticks_used = 0;
    proc->sched_cpu = pick_least_loaded_cpu();

    percpu_runqueue_t* rq = &g_runqueues[proc->sched_cpu];
    uint64_t flags = spinlock_acquire(&rq->lock);
    rq_push(rq, proc, SCHED_LEVEL_INTERACTIVE);
    spinlock_release(&rq->lock, flags);
}

void scheduler_enqueue_existing(process_t* proc) {
    if (!proc) return;

    if (proc->sched_cpu >= MAX_CPUS) {
        // Never assigned (shouldn't happen once proc_create() always
        // calls scheduler_enqueue_new(), but stay safe) - treat as new.
        scheduler_enqueue_new(proc);
        return;
    }

    proc->sched_level = SCHED_LEVEL_INTERACTIVE;
    proc->sched_ticks_used = 0;

    percpu_runqueue_t* rq = &g_runqueues[proc->sched_cpu];
    uint64_t flags = spinlock_acquire(&rq->lock);
    rq_push(rq, proc, SCHED_LEVEL_INTERACTIVE);
    spinlock_release(&rq->lock, flags);
}

void scheduler_dequeue(process_t* proc) {
    if (!proc) return;
    if (proc->sched_cpu >= MAX_CPUS) return;

    percpu_runqueue_t* rq = &g_runqueues[proc->sched_cpu];
    uint64_t flags = spinlock_acquire(&rq->lock);

    for (uint32_t level = 0; level < SCHED_NUM_LEVELS; level++) {
        process_t* prev = NULL;
        process_t* curr = rq->head[level];

        while (curr) {
            if (curr == proc) {
                if (prev) prev->rq_next = curr->rq_next;
                else      rq->head[level] = curr->rq_next;

                if (rq->tail[level] == curr) rq->tail[level] = prev;

                rq->count[level]--;
                curr->rq_next = NULL;
                spinlock_release(&rq->lock, flags);
                return;
            }
            prev = curr;
            curr = curr->rq_next;
        }
    }

    spinlock_release(&rq->lock, flags);
}

void scheduler_rebalance(void) {
    uint32_t online = smp_online_cpu_count();
    if (online < 2 || online > MAX_CPUS) return;

    uint32_t busiest_cpu = 0, idlest_cpu = 0;
    uint32_t busiest_load = 0, idlest_load = 0xFFFFFFFFu;

    for (uint32_t cpu = 0; cpu < online; cpu++) {
        uint64_t flags = spinlock_acquire(&g_runqueues[cpu].lock);
        uint32_t load = rq_total(&g_runqueues[cpu]);
        spinlock_release(&g_runqueues[cpu].lock, flags);

        if (load > busiest_load) { busiest_load = load; busiest_cpu = cpu; }
        if (load < idlest_load)  { idlest_load  = load; idlest_cpu  = cpu; }
    }

    // Don't shuffle over a one-process difference every interval -
    // only move something if the imbalance is real.
    if (busiest_cpu == idlest_cpu || busiest_load < idlest_load + 2) return;

    percpu_runqueue_t* src = &g_runqueues[busiest_cpu];
    percpu_runqueue_t* dst = &g_runqueues[idlest_cpu];

    // Fixed lock ordering (lower cpu id first) so two cores rebalancing
    // opposite pairs at the same time can never deadlock against
    // each other ABBA-style.
    percpu_runqueue_t* first  = (busiest_cpu < idlest_cpu) ? src : dst;
    percpu_runqueue_t* second = (busiest_cpu < idlest_cpu) ? dst : src;

    uint64_t f1 = spinlock_acquire(&first->lock);
    uint64_t f2 = spinlock_acquire(&second->lock);

    // Prefer donating background/normal work over interactive work, so
    // rebalancing never hurts the busy core's input latency.
    process_t* migrant = NULL;
    for (int level = SCHED_NUM_LEVELS - 1; level >= 0 && !migrant; level--) {
        migrant = rq_pop(src, (uint32_t)level);
    }

    if (migrant) {
        migrant->sched_cpu = idlest_cpu;
        rq_push(dst, migrant, migrant->sched_level);

        if (SCHED_DEBUG >= 1) {
            serial_write_str("[REBALANCE] ");
            serial_write_str(migrant->name);
            serial_write_str(" cpu");
            serial_write_dec(busiest_cpu);
            serial_write_str(" -> cpu");
            serial_write_dec(idlest_cpu);
            serial_write_str("\n");
        }
    }

    spinlock_release(&second->lock, f2);
    spinlock_release(&first->lock, f1);
}

void scheduler_priority_boost(void) {
    uint32_t online = smp_online_cpu_count();
    if (online > MAX_CPUS) online = MAX_CPUS;

    for (uint32_t cpu = 0; cpu < online; cpu++) {
        percpu_runqueue_t* rq = &g_runqueues[cpu];
        uint64_t flags = spinlock_acquire(&rq->lock);

        for (uint32_t level = 1; level < SCHED_NUM_LEVELS; level++) {
            process_t* proc;
            while ((proc = rq_pop(rq, level)) != NULL) {
                proc->sched_level = SCHED_LEVEL_INTERACTIVE;
                proc->sched_ticks_used = 0;
                rq_push(rq, proc, SCHED_LEVEL_INTERACTIVE);
            }
        }

        spinlock_release(&rq->lock, flags);
    }

    if (SCHED_DEBUG >= 1) {
        serial_write_str("[BOOST] priority boost applied\n");
    }
}

// ===========================================
// GENERAL SCHEDULER STATE
// ===========================================

// Call to yield current process (mark ready)
void ready() {
    if (current_process)
        current_process->state = PROCESS_READY;
}

void schedulerInit() {
    scheduler_on = true;
}

// Wake up sleeping processes whose time has come.
//
// Every online core's scheduler_tick() calls this once per tick, so
// this still does an O(n) walk of the *entire* global process list on
// every core, every tick - the filter below (`sched_cpu != this_cpu`)
// only stops a core from fighting over another core's run-queue lock
// for a process it doesn't own; it doesn't avoid the list walk itself.
// A real fix (per-core sleep lists, or a shared timer wheel) is future
// work - flagging it rather than pretending this is free.
static void wake_sleeping_processes() {
    uint64_t current_time = time_get_uptime_ms();
    uint32_t this_cpu = smp_current_cpu_id();

    for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
        if (proc->state == PROCESS_WAITING && proc->wake_time_ms > 0) {
            if (current_time >= proc->wake_time_ms) {
                if (proc->sched_cpu != this_cpu) continue;

                proc->state = PROCESS_READY;
                proc->wake_time_ms = 0;

                // Blocking (sleeping) is the classic I/O-bound signal -
                // reward it with a return to the interactive level,
                // same as scheduler_enqueue_existing() does for unpause.
                proc->sched_level = SCHED_LEVEL_INTERACTIVE;
                proc->sched_ticks_used = 0;

                percpu_runqueue_t* rq = &g_runqueues[this_cpu];
                uint64_t flags = spinlock_acquire(&rq->lock);
                rq_push(rq, proc, SCHED_LEVEL_INTERACTIVE);
                spinlock_release(&rq->lock, flags);

                if (SCHED_DEBUG >= 2) {
                    uint64_t now = time_get_uptime_ms();
                    static uint64_t last_wake_log = 0;
                    if (now - last_wake_log > 1000) { // max once per second
                        serial_write_str("[WAKE]\n");
                        last_wake_log = now;
                    }
                }
            }
        }
    }
}

// Print current process state (for debugging)
void currentstate() {
    if (!current_process) {
        print_str("No process\n");
        return;
    }

    switch (current_process->state) {
        case PROCESS_WAITING:    print_char('W'); break;
        case PROCESS_TERMINATED: print_char('T'); break;
        case PROCESS_RUNNING:    print_char('R'); break;
        case PROCESS_READY:      print_str("RR"); break;
        case PROCESS_ZOMBIE:     print_char('Z'); break;
        default: {
            char state_str[21];
            uint_to_str((uint64_t)current_process->state, state_str);
            PANIC(state_str);
        }
    }
}

/*
 * Main scheduling function.
 *
 * Two very different pieces of state are involved, deliberately kept
 * under two different locks now:
 *
 *  1. The global creation/cleanup list (proc_list_head) - shared
 *     across every core, still protected by g_sched_lock. Only the
 *     zombie/terminated reap pass below touches it from here.
 *
 *  2. THIS CORE's own ready queue (g_runqueues[cpu_id]) - the actual
 *     hot dispatch path. Protected by that queue's own per-CPU
 *     spinlock, which in the common case is never contended by any
 *     other core, since a process only ever lives on one core's queue
 *     at a time (see scheduler_enqueue_new()/scheduler_rebalance()).
 *
 * That split is the whole point of this rewrite: previously every
 * core's schedule() call fought over one single global lock for every
 * scheduling decision, not just for cleanup. Now that contention only
 * happens for the comparatively rare zombie-reap pass and for the
 * periodic rebalance.
 */
void schedule() {
    uint32_t cpu_id = smp_current_cpu_id();
    percpu_runqueue_t* rq = &g_runqueues[cpu_id];

    // ---- Zombie / terminated cleanup (global list, cross-core) ----
    uint64_t sched_flags = scheduler_lock();
    process_t* prev = NULL;
    process_t* curr = proc_list_head;

    while (curr) {
        if (curr->state == PROCESS_ZOMBIE || curr->state == PROCESS_TERMINATED) {
            if (curr == current_process) {
                prev = curr;
                curr = curr->next;
                continue;
            }

            if (SCHED_DEBUG >= 1) {
                serial_write_str("Cleaning: ");
                serial_write_str(curr->name);
                serial_write_str("\n");
            }

            process_t* to_free = curr;

            if (prev) {
                prev->next = curr->next;
            } else {
                proc_list_head = curr->next;
            }

            if (to_free->kernel_stack) {
                void* stack_base = (void*)((uint8_t*)to_free->kernel_stack - STACK_SIZE);
                free_mem(stack_base);
            }

            free_mem(to_free);

            curr = (prev) ? prev->next : proc_list_head;
            continue;
        }
        prev = curr;
        curr = curr->next;
    }
    scheduler_unlock(sched_flags);

    // ---- Per-CPU dispatch ----
    process_t* old = current_process;

    uint64_t rq_flags = spinlock_acquire(&rq->lock);

    process_t* next = NULL;
    for (;;) {
        next = rq_pop_highest(rq);
        if (!next) break;

        if (next->state == PROCESS_ZOMBIE || next->state == PROCESS_TERMINATED) {
            // Killed while sitting ready - normally scheduler_dequeue()
            // (called from kill()/killProcess()) removes it before this
            // can happen, but drop it defensively either way. The
            // cleanup pass above (or a later one) frees it once it's
            // no longer anyone's current_process.
            continue;
        }
        if (next->state != PROCESS_READY) {
            // Defensive: PAUSED processes should already have been
            // removed via scheduler_dequeue() in pauseProcess(). Never
            // dispatch something that isn't actually READY.
            continue;
        }
        break;
    }

    if (!next) {
        // Nothing else ready on this core this tick.
        spinlock_release(&rq->lock, rq_flags);

        if (old && (old->state == PROCESS_RUNNING || old->state == PROCESS_READY)) {
            old->state = PROCESS_RUNNING;
            current_process = old;
            return;
        }

        idle_cycles++;
        return;
    }

    // Requeue the outgoing process (it was never itself in the queue
    // while running) behind whatever's now at the front of its level.
    if (old && old->state == PROCESS_RUNNING) {
        old->state = PROCESS_READY;
        rq_push(rq, old, old->sched_level);
    }
    spinlock_release(&rq->lock, rq_flags);

    next->sched_ticks_used = 0;
    next->state = PROCESS_RUNNING;
    current_process = next;

    if (!old) {
        // First process this core has ever run - nothing to save,
        // nothing to switch away from.
        if (SCHED_DEBUG >= 1) {
            serial_write_str("Starting first process on cpu");
            serial_write_dec(cpu_id);
            serial_write_str(": ");
            serial_write_str(next->name);
            serial_write_str("\n");
        }
        return;
    }

    total_context_switches++;

    if (SCHED_DEBUG >= 1) {
        serial_write_str("CS ");
        serial_write_dec(total_context_switches);
        serial_write_str(" (cpu");
        serial_write_dec(cpu_id);
        serial_write_str("): ");
        serial_write_str(old->name);
        serial_write_str(" -> ");
        serial_write_str(next->name);
        serial_write_str("\n");
    }

    context_switch(old, next);
}

/*
 * Busy-wait sleep used only when there is no current_process yet.
 * Bounded and guaranteed-terminating - see the identical pattern in
 * ahci_sleep_ms()/minimafs_sleep_ms() for why it never uses hlt.
 */
static void sleep_busy_wait_ms(uint64_t milliseconds) {
    if (milliseconds == 0) return;

    uint64_t start = time_get_uptime_ms();
    const uint32_t MAX_POLL_ITERATIONS = 20000000u;
    uint32_t iterations = 0;

    while ((time_get_uptime_ms() - start) < milliseconds) {
        asm volatile("pause" ::: "memory");
        if (++iterations >= MAX_POLL_ITERATIONS) {
            break;
        }
    }

    if ((time_get_uptime_ms() - start) < milliseconds) {
        for (volatile uint32_t i = 0; i < (milliseconds * 100000u); i++) {
            asm volatile("nop");
        }
    }
}

void sleep(uint64_t milliseconds) {
    if (milliseconds == 0) return;

    if (!current_process) {
        sleep_busy_wait_ms(milliseconds);
        return;
    }

    uint64_t wake_time = time_get_uptime_ms() + milliseconds;
    current_process->wake_time_ms = wake_time;
    current_process->state = PROCESS_WAITING;

    if (SCHED_DEBUG >= 2) {
        serial_write_str("[SLEEP] ");
        serial_write_str(current_process->name);
        serial_write_str(" for ");
        serial_write_dec(milliseconds);
        serial_write_str(" ms\n");
    }

    schedule();

    if (SCHED_DEBUG >= 2) {
        serial_write_str("[WAKE-UP] ");
        serial_write_str(current_process->name);
        serial_write_str("\n");
    }
}

// Shared across every core's LAPIC-timer-driven scheduler_tick() call,
// so the boost/rebalance intervals below fire exactly once globally
// per interval rather than once per interval PER CORE.
static volatile uint64_t g_global_tick_count = 0;

void scheduler_tick() {
    if (!scheduler_on) {
        return;
    }

    wake_sleeping_processes();

    bool was_idle = current_process == NULL;
    if (was_idle) {
        schedule();
    }

    uint32_t cpu_id = smp_current_cpu_id();
    if (cpu_id < MAX_CPUS) {
        cpu_local_t* cpu = &g_cpus[cpu_id];
        bool busy = current_process != NULL;
        __atomic_add_fetch(&cpu->usage_total_ticks, 1, __ATOMIC_RELAXED);
        if (busy) {
            __atomic_add_fetch(&cpu->usage_busy_ticks, 1, __ATOMIC_RELAXED);
        }
        cpu->usage_window_ticks++;
        if (busy) cpu->usage_window_busy_ticks++;
        if (cpu->usage_window_ticks >= 100) {
            uint32_t percent = (cpu->usage_window_busy_ticks * 100) /
                               cpu->usage_window_ticks;
            __atomic_store_n(&cpu->usage_last_percent, percent,
                             __ATOMIC_RELAXED);
            cpu->usage_window_ticks = 0;
            cpu->usage_window_busy_ticks = 0;
        }
    }

    if (was_idle || !current_process) return;

    current_process->sched_ticks_used++;

    uint64_t my_tick = __sync_add_and_fetch(&g_global_tick_count, 1);

    if ((my_tick % SCHED_BOOST_INTERVAL_TICKS) == 0) {
        scheduler_priority_boost();
    }
    if ((my_tick % SCHED_REBALANCE_INTERVAL_TICKS) == 0) {
        scheduler_rebalance();
    }

    uint32_t level = current_process->sched_level;
    if (level >= SCHED_NUM_LEVELS) level = SCHED_NUM_LEVELS - 1; // defensive

    if (current_process->sched_ticks_used >= sched_level_quantum_ticks[level]) {
        // Used its whole slice without blocking - looks CPU-bound, so
        // push it down a level. Background work still runs, just less
        // eagerly than fresh or freshly-woken work.
        if (current_process->sched_level < SCHED_NUM_LEVELS - 1) {
            current_process->sched_level++;
        }
        current_process->sched_ticks_used = 0;
        schedule();
    }
}

void scheduler_get_stats(uint64_t* switches, uint64_t* idle) {
    if (switches) *switches = total_context_switches;
    if (idle) *idle = idle_cycles;
}

void scheduler_print_stats() {
    print_str("=== Scheduler Statistics ===\n");
    print_str("Context switches: ");
    print_uint64_dec(total_context_switches);
    print_str("\nIdle cycles: ");
    print_uint64_dec(idle_cycles);
    print_str("\n");

    int ready = 0, running = 0, waiting = 0, zombie = 0;
    for (process_t* p = proc_list_head; p != NULL; p = p->next) {
        switch (p->state) {
            case PROCESS_READY: ready++; break;
            case PROCESS_RUNNING: running++; break;
            case PROCESS_WAITING: waiting++; break;
            case PROCESS_ZOMBIE: zombie++; break;
            default: break;
        }
    }

    print_str("Processes - Ready: ");
    print_int(ready);
    print_str(", Running: ");
    print_int(running);
    print_str(", Waiting: ");
    print_int(waiting);
    print_str(", Zombie: ");
    print_int(zombie);
    print_str("\n============================\n");
}

void process_exit(void) {
    if (!current_process) {
        PANIC("process_exit() called with no current process");
    }

    serial_write_str("[PROC] Exiting process: ");
    serial_write_str(current_process->name);
    serial_write_str("\n");

    current_process->state = PROCESS_TERMINATED;
    schedule();

    PANIC("process_exit() returned!");
}