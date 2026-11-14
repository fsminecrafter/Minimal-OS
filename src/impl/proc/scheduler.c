#include "x86_64/uvm.h"
#include "x86_64/audio_streams.h"
#include "x86_64/fgpu.h"
// src/impl/proc/scheduler.c
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "x86_64/proc.h"
#include "x86_64/window_manager.h"
#include "graphics.h"
#include "x86_64/scheduler.h"
#include "x86_64/wm_watchdog.h"
#include "x86_64/commandhandler.h"
#include "x86_64/gdt.h"
#include "time.h"
#include "panic.h"
#include "print.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "serial.h"
#include "x86_64/safeints.h"
#include "x86_64/spinlock.h"
#include "x86_64/boot_options.h"
#include "x86_64/waitq.h"
#include "x86_64/spawn.h"
#include "x86_64/tlb.h"

#define SCHED_DEBUG 0   // 0 = off, 1 = important, 2 = verbose

bool scheduler_on = false;

// Linked list of processes
extern process_t* proc_list_head;

// Forward declaration of context switch assembly routine
extern void context_switch(process_t* current, process_t* next,
                           volatile uint32_t* current_on_cpu);

// FS base each CPU has loaded (IA32_FS_BASE). User threads keep their TLS
// pointer there; the kernel itself never uses FS, so it is only reloaded
// when switching to a user process whose value differs.
#define IA32_FS_BASE_MSR 0xC0000100u
static uint64_t g_cpu_fs_base[MAX_CPUS];

static inline void write_fs_base(uint64_t value) {
    asm volatile("wrmsr" :: "c"(IA32_FS_BASE_MSR), "a"((uint32_t)value),
                 "d"((uint32_t)(value >> 32)) : "memory");
}

static void load_fs_base_for(const process_t* next) {
    if (!next || next->privilege != PROC_PRIVILEGE_USER) return;
    uint32_t cpu = smp_current_cpu_id();
    if (cpu >= MAX_CPUS || g_cpu_fs_base[cpu] == next->fs_base) return;
    write_fs_base(next->fs_base);
    g_cpu_fs_base[cpu] = next->fs_base;
}

void scheduler_set_fs_base(process_t* self, uint64_t value) {
    if (!self) return;
    self->fs_base = value;
    if (self == current_process) {
        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        uint32_t cpu = smp_current_cpu_id();
        write_fs_base(value);
        if (cpu < MAX_CPUS) g_cpu_fs_base[cpu] = value;
        if (flags & 0x200) asm volatile("sti" ::: "memory");
    }
}

// Marks `next` as live on this CPU; `current` is released by the assembly
// once its context is saved and its stack no longer in use.
static void switch_to(process_t* current, process_t* next) {
    next->on_cpu = 1;
    load_fs_base_for(next);
    // Keeps each address space's mask of CPUs that have it loaded (the
    // CPUs a TLB shootdown has to reach).
    uvm_cpu_switch(current, next);
    context_switch(current, next, current ? &current->on_cpu : NULL);
}

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
static process_t* g_deferred_reap[MAX_CPUS];

// In scheduler ticks, which are 1 ms each (see pit_init(1000) in
// startuproutine.c; the APs' LAPIC timers are calibrated to the same rate).
const uint32_t sched_level_quantum_ticks[SCHED_NUM_LEVELS] = {
    20,   // interactive: short quantum, dispatched first
    50,   // normal
    100,  // background: long quantum, only runs once higher levels are empty
};

// How often (in scheduler ticks) to run the anti-starvation boost and
// the cross-core rebalance pass. Gated on a single shared counter (see
// g_global_tick_count below) so these fire exactly once per interval
// total, not once per interval PER CORE - every online core's LAPIC
// timer calls scheduler_tick() independently.
#define SCHED_BOOST_INTERVAL_TICKS      20000
#define SCHED_REBALANCE_INTERVAL_TICKS  200

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

// The best queued process that no CPU is still switching away from (a
// process another CPU queued for us while its registers and stack are
// still live over there must wait; `self` is the one running here).
static process_t* rq_pop_runnable(percpu_runqueue_t* rq, process_t* self) {
    for (uint32_t level = 0; level < SCHED_NUM_LEVELS; level++) {
        process_t* prev = NULL;
        for (process_t* p = rq->head[level]; p; prev = p, p = p->rq_next) {
            if (p->on_cpu && p != self) continue;
            if (prev) prev->rq_next = p->rq_next;
            else      rq->head[level] = p->rq_next;
            if (rq->tail[level] == p) rq->tail[level] = prev;
            rq->count[level]--;
            p->rq_next = NULL;
            return p;
        }
    }
    return NULL;
}

static uint32_t rq_total(percpu_runqueue_t* rq) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < SCHED_NUM_LEVELS; i++) total += rq->count[i];
    return total;
}

// Affinity: a mask of CPUs the process may run on (0 = any).
bool scheduler_cpu_allowed(const process_t* p, uint32_t cpu) {
    if (!p || !p->affinity) return true;
    return cpu < 64 && ((p->affinity >> cpu) & 1u);
}

// What a CPU has to do: programs (at any level, a busy one sinks to the
// background level) and kernel processes above the background level, queued
// or running. A kernel process spinning at the background level (the boot
// core's idle loop, a polling driver) yields to any user work, so it does
// not make its CPU look busy.
static bool counts_as_load(const process_t* p, uint32_t level) {
    if (!p || scheduler_is_idle_process(p)) return false;
    return p->privilege == PROC_PRIVILEGE_USER || level < SCHED_LEVEL_BACKGROUND;
}

static uint32_t cpu_load(uint32_t cpu, const process_t* discount) {
    uint32_t load = 0;
    uint64_t flags = spinlock_acquire(&g_runqueues[cpu].lock);
    for (uint32_t level = 0; level < SCHED_NUM_LEVELS; level++)
        for (process_t* p = g_runqueues[cpu].head[level]; p; p = p->rq_next)
            if (counts_as_load(p, level)) load++;
    spinlock_release(&g_runqueues[cpu].lock, flags);
    process_t* running = smp_current_process_for_cpu(cpu);
    if (running && running != discount && running->state == PROCESS_RUNNING &&
        counts_as_load(running, running->sched_level))
        load++;
    return load;
}

static uint32_t pick_least_loaded_cpu_for(const process_t* proc, const uint32_t* group_on_cpu) {
    // A thread is usually created by a thread of its own program that then
    // waits for it (join): do not count the creator as load, or its CPU
    // looks as busy as one that is about to run a new worker.
    const process_t* discount = (proc && proc->mem_owner && current_process &&
                                 proc_mem_owner(current_process) == proc->mem_owner)
                                    ? current_process : NULL;
    uint32_t online = smp_online_cpu_count();
    if (online == 0) online = 1;
    if (online > MAX_CPUS) online = MAX_CPUS;

    uint32_t best_cpu = 0;
    uint32_t best_load = 0xFFFFFFFFu;
    bool any_allowed = false;
    for (uint32_t cpu = 0; cpu < online; cpu++)
        if (scheduler_cpu_allowed(proc, cpu)) any_allowed = true;

    for (uint32_t cpu = 0; cpu < online; cpu++) {
        if (any_allowed && !scheduler_cpu_allowed(proc, cpu)) continue;
        // Threads of one program first spread over the CPUs, whatever else
        // runs there; the load only breaks ties.
        uint32_t load = cpu_load(cpu, discount) + (group_on_cpu ? group_on_cpu[cpu] * 64u : 0u);
        if (load < best_load) {
            best_load = load;
            best_cpu = cpu;
        }
    }
    return best_cpu;
}

static inline uint32_t prio_level(const process_t* p) {
    return p->sched_prio < SCHED_NUM_LEVELS ? p->sched_prio : SCHED_NUM_LEVELS - 1;
}

// An idle AP only looks at its run queue at its next tick (up to 1 ms
// away); work that arrives for it gets an IPI so it starts at once.
static void kick_if_idle(uint32_t cpu) {
    if (cpu >= MAX_CPUS || smp_is_master_core_id(cpu) || !g_cpus[cpu].online) return;
    if (cpu == smp_current_cpu_id()) return;
    process_t* cur = smp_current_process_for_cpu(cpu);
    if (!cur || scheduler_is_idle_process(cur)) smp_send_resched(cpu);
}

// Queues a not-yet-queued process on `cpu`. With require_ready the push is
// skipped (false) when it was killed meanwhile: checked under the queue's
// lock, which kill()'s state change takes too.
static bool enqueue_on(process_t* proc, uint32_t cpu, bool require_ready) {
    percpu_runqueue_t* rq = &g_runqueues[cpu];
    uint64_t flags = spinlock_acquire(&rq->lock);
    bool ok = !require_ready || proc->state == PROCESS_READY;
    if (ok) {
        proc->sched_level = prio_level(proc);
        proc->sched_ticks_used = 0;
        proc->sched_cpu = cpu;
        rq_push(rq, proc, proc->sched_level);
    }
    spinlock_release(&rq->lock, flags);
    if (ok) kick_if_idle(cpu);
    return ok;
}

void scheduler_start_thread(process_t* t, process_t* owner) {
    if (!t) return;
    process_state_t expected = PROCESS_PAUSED;
    // Killed while it was being set up: stays dead, never queued.
    if (!__atomic_compare_exchange_n(&t->state, &expected, PROCESS_READY, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;
    uint32_t cpu;
    if (kernel_boot_threads_multicore && owner && owner->sched_cpu < MAX_CPUS) {
        // How many live threads (and the owner) of the program each CPU
        // already has, not counting the creating thread: it usually waits.
        uint32_t group[MAX_CPUS];
        memset(group, 0, sizeof(group));
        uint64_t lf = scheduler_lock();
        for (process_t* p = proc_list_head; p; p = p->next) {
            if (p == t || p == current_process || proc_mem_owner(p) != owner) continue;
            if (p->state != PROCESS_READY && p->state != PROCESS_RUNNING) continue;
            if (p->sched_cpu < MAX_CPUS) group[p->sched_cpu]++;
        }
        scheduler_unlock(lf);
        cpu = pick_least_loaded_cpu_for(t, group);
    } else {
        cpu = (!owner || owner->sched_cpu >= MAX_CPUS) ? pick_least_loaded_cpu_for(t, NULL)
                                                      : owner->sched_cpu;
    }
    if (!enqueue_on(t, cpu, true)) return;
}

void scheduler_enqueue_new(process_t* proc) {
    if (!proc) return;

    proc->sched_level = prio_level(proc);
    proc->sched_ticks_used = 0;
    proc->sched_cpu = pick_least_loaded_cpu_for(proc, NULL);
    if (proc->privilege == PROC_PRIVILEGE_USER) {
        serial_write_str("SCHED: enqueue user pid=");
        serial_write_dec(proc->pid);
        serial_write_str(" cpu=");
        serial_write_dec(proc->sched_cpu);
        serial_write_str(" state=");
        serial_write_dec(proc->state);
        serial_write_str("\n");
    }

    percpu_runqueue_t* rq = &g_runqueues[proc->sched_cpu];
    uint64_t flags = spinlock_acquire(&rq->lock);
    rq_push(rq, proc, proc->sched_level);
    spinlock_release(&rq->lock, flags);
    kick_if_idle(proc->sched_cpu);
}

void scheduler_enqueue_on_cpu(process_t* proc, uint32_t cpu) {
    if (!proc) return;
    if (cpu >= MAX_CPUS) { scheduler_enqueue_new(proc); return; }
    enqueue_on(proc, cpu, false);
}

void scheduler_enqueue_existing(process_t* proc) {
    if (!proc) return;

    if (proc->sched_cpu >= MAX_CPUS) {
        // Never assigned (shouldn't happen once proc_create() always
        // calls scheduler_enqueue_new(), but stay safe) - treat as new.
        scheduler_enqueue_new(proc);
        return;
    }

    proc->sched_level = prio_level(proc);
    proc->sched_ticks_used = 0;

    percpu_runqueue_t* rq = &g_runqueues[proc->sched_cpu];
    uint64_t flags = spinlock_acquire(&rq->lock);
    rq_push(rq, proc, proc->sched_level);
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

// Takes `proc` off `rq` if it is queued there. Caller holds rq->lock.
static bool rq_remove_locked(percpu_runqueue_t* rq, process_t* proc) {
    for (uint32_t level = 0; level < SCHED_NUM_LEVELS; level++) {
        process_t* prev = NULL;
        for (process_t* curr = rq->head[level]; curr; prev = curr, curr = curr->rq_next) {
            if (curr != proc) continue;
            if (prev) prev->rq_next = curr->rq_next;
            else      rq->head[level] = curr->rq_next;
            if (rq->tail[level] == curr) rq->tail[level] = prev;
            rq->count[level]--;
            curr->rq_next = NULL;
            return true;
        }
    }
    return false;
}

// WAITING -> READY and onto the run queue as one step under the queue's
// lock. The dispatcher and scheduler_resume_self() look at the state under
// the same lock, so "READY because it was woken" always means "queued":
// a process can never end up running and queued at once (a second wake
// would then link it into the queue twice and corrupt the list).
bool scheduler_wake(process_t* proc) {
    if (!proc) return false;
    // A waiting process is on no queue, so nothing moves it between CPUs.
    uint32_t cpu = proc->sched_cpu < MAX_CPUS ? proc->sched_cpu : 0;
    // Its affinity changed while it slept: wake it on a CPU it may use. Not
    // while it is still switching away from its old one (on_cpu): it then
    // sits on that CPU's queue and the tick moves it.
    bool relocate = !scheduler_cpu_allowed(proc, cpu) && !proc->on_cpu;
    if (relocate) cpu = pick_least_loaded_cpu_for(proc, NULL);
    percpu_runqueue_t* rq = &g_runqueues[cpu];
    bool woke = false;
    uint64_t flags = spinlock_acquire(&rq->lock);
    process_state_t expected = PROCESS_WAITING;
    if (__atomic_compare_exchange_n(&proc->state, &expected, PROCESS_READY,
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        proc->wake_time_ms = 0;
        if (relocate) proc->sched_cpu = cpu;
        // Woken from a wait: back to the interactive level (or its priority
        // floor), like a sleep.
        proc->sched_level = prio_level(proc);
        proc->sched_ticks_used = 0;
        rq_push(rq, proc, proc->sched_level);
        woke = true;
    }
    spinlock_release(&rq->lock, flags);
    if (woke) kick_if_idle(cpu);
    return woke;
}

void scheduler_resume_self(void) {
    process_t* self = current_process;
    if (!self) return;
    uint32_t cpu = self->sched_cpu < MAX_CPUS ? self->sched_cpu : smp_current_cpu_id();
    percpu_runqueue_t* rq = &g_runqueues[cpu];
    uint64_t flags = spinlock_acquire(&rq->lock);
    process_state_t state = __atomic_load_n(&self->state, __ATOMIC_SEQ_CST);
    if (state == PROCESS_WAITING) {
        // schedule() came straight back and nobody woke it meanwhile.
        self->wake_time_ms = 0;
        __atomic_store_n(&self->state, PROCESS_RUNNING, __ATOMIC_SEQ_CST);
    } else if (state == PROCESS_READY) {
        // Woken while it was still on this CPU: it sits on the queue.
        // Take it back off before it runs on.
        rq_remove_locked(rq, self);
        __atomic_store_n(&self->state, PROCESS_RUNNING, __ATOMIC_SEQ_CST);
    }
    // RUNNING already (it was dispatched), or killed: leave it.
    spinlock_release(&rq->lock, flags);
}

void scheduler_rebalance(void) {
    uint32_t online = smp_online_cpu_count();
    if (online < 2 || online > MAX_CPUS) return;

    uint32_t busiest_cpu = 0, idlest_cpu = 0;
    uint32_t busiest_load = 0, idlest_load = 0xFFFFFFFFu;

    for (uint32_t cpu = 0; cpu < online; cpu++) {
        // Queued work plus the running process (sleeping or idle ones do
        // not count, nor does a kernel process spinning in the background).
        uint32_t load = cpu_load(cpu, NULL);

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

    // Prefer the least-used queued process. The large running process stays
    // on its core while a smaller waiting process migrates away.
    process_t* migrant = NULL;
    uint32_t migrant_level = 0;
    uint64_t migrant_cpu_time = UINT64_MAX;
    for (uint32_t level = 0; level < SCHED_NUM_LEVELS; level++) {
        for (process_t* candidate = src->head[level]; candidate;
             candidate = candidate->rq_next) {
            // Just requeued by a core that has not finished switching
            // away from it yet: its saved registers are stale and its
            // stack is still in use there. Moving it now let two cores
            // run on one kernel stack (a #GP on iretq a few seconds after
            // starting an app).
            if (candidate->on_cpu) continue;
            // Only user programs move. Kernel processes stay on the core
            // they were created on: "busy" is the boot core's idle task
            // (cpu0 has no idle context), and with it moved away a
            // sleeping process on cpu0 found nothing to switch to, so
            // schedule() returned into it still marked WAITING. mosWM
            // then got parked with no wake-up: the desktop froze when an
            // app was opened (the app's load triggered the rebalance).
            if (candidate->privilege != PROC_PRIVILEGE_USER) continue;
            // With 'threads1cpu' a program with threads stays on one CPU.
            if (!kernel_boot_threads_multicore &&
                (candidate->mem_owner || candidate->thread_count)) continue;
            if (!scheduler_cpu_allowed(candidate, idlest_cpu)) continue;
            if (candidate->cpu_time_ms < migrant_cpu_time) {
                migrant = candidate;
                migrant_level = level;
                migrant_cpu_time = candidate->cpu_time_ms;
            }
        }
    }

    if (migrant) {
        process_t* previous = NULL;
        process_t* candidate = src->head[migrant_level];
        while (candidate != migrant) {
            previous = candidate;
            candidate = candidate->rq_next;
        }
        if (previous) previous->rq_next = migrant->rq_next;
        else src->head[migrant_level] = migrant->rq_next;
        if (src->tail[migrant_level] == migrant) src->tail[migrant_level] = previous;
        src->count[migrant_level]--;
        migrant->rq_next = NULL;
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
    if (migrant) kick_if_idle(idlest_cpu);
}

void scheduler_priority_boost(void) {
    uint32_t online = smp_online_cpu_count();
    if (online > MAX_CPUS) online = MAX_CPUS;

    for (uint32_t cpu = 0; cpu < online; cpu++) {
        percpu_runqueue_t* rq = &g_runqueues[cpu];
        uint64_t flags = spinlock_acquire(&rq->lock);

        for (uint32_t level = 1; level < SCHED_NUM_LEVELS; level++) {
            process_t* proc;
            // Everything goes to the interactive level for a while, the
            // priority floor included: that is what keeps a LOW or NORMAL
            // thread from starving behind busy higher levels. It sinks
            // again as it uses up its slices; a wake-up returns it to its
            // floor (prio_level).
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

    // Under the list lock: other CPUs reap (free) processes from the list.
    uint64_t list_flags = scheduler_lock();
    for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
        if (proc->state == PROCESS_WAITING && proc->wake_time_ms > 0) {
            if (current_time >= proc->wake_time_ms) {
                if (proc->sched_cpu != this_cpu) continue;

                // Blocking (sleeping) is the classic I/O-bound signal -
                // reward it with a return to the interactive level,
                // same as scheduler_enqueue_existing() does for unpause.
                // A wait queue wake-up from another CPU may race with this
                // timeout: scheduler_wake() lets only one of the two move
                // it out of WAITING and queue it.
                if (!scheduler_wake(proc)) continue;

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
    scheduler_unlock(list_flags);
}

static bool process_is_current_on_any_cpu(process_t* proc) {
    if (!proc) return false;

    uint32_t cpu_count = g_cpu_count;
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    for (uint32_t i = 0; i < cpu_count; i++) {
        if (g_cpus[i].online && smp_current_process_for_cpu(i) == proc)
            return true;
    }

    return false;
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
/*
 * Idle context of each AP. The AP's boot code (the hlt loop at the end of
 * ap_entry_c) is parked here the first time the core schedules, and the
 * core switches back to it whenever nothing on its run queue can run, so
 * sleep() really waits there instead of spinning. It is never queued,
 * never on proc_list_head and never migrated. The boot core has no idle
 * context: its boot code (the terminal) runs as a normal process.
 */
static process_t g_ap_idle[MAX_CPUS];

static inline process_t* ap_idle_for(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS || smp_is_master_core_id(cpu_id)) return NULL;
    process_t* idle = &g_ap_idle[cpu_id];
    if (!idle->name[0]) {
        strcpy(idle->name, "idle");
        idle->sched_cpu = cpu_id;
        idle->sched_level = SCHED_LEVEL_BACKGROUND;
        // First call happens in the AP's boot context, which runs on the
        // kernel page tables; context_switch() loads this when switching
        // back to idle, so it must never stay 0.
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        idle->pml4 = cr3;
    }
    return idle;
}

bool scheduler_is_idle_process(const process_t* proc) {
    return proc && proc >= &g_ap_idle[0] && proc < &g_ap_idle[MAX_CPUS];
}

// Puts a READY process that is still running here (about to be switched
// away from) on the queue of a CPU its affinity allows. The other CPU skips
// it until this one is done with its stack (on_cpu).
static void requeue_elsewhere(process_t* p) {
    uint32_t dest = pick_least_loaded_cpu_for(p, NULL);
    percpu_runqueue_t* rq = &g_runqueues[dest];
    uint64_t flags = spinlock_acquire(&rq->lock);
    // Killed meanwhile (the state changes under this lock): never queue it.
    bool queue = p->state == PROCESS_READY;
    if (queue) {
        p->sched_cpu = dest;
        rq_push(rq, p, p->sched_level);
    }
    spinlock_release(&rq->lock, flags);
    if (queue) kick_if_idle(dest);
}

static void schedule_irqs_off(void) {
    uint32_t cpu_id = smp_current_cpu_id();
    percpu_runqueue_t* rq = &g_runqueues[cpu_id];

    // ---- Zombie / terminated cleanup (global list, cross-core) ----
    // Parents waiting in SYS_WAIT for the processes reaped below; woken
    // once the list lock is released.
    uint64_t wake_parents[16];
    uint32_t wake_count = 0;
    uint64_t sched_flags = scheduler_lock();
    process_t* deferred_reap = g_deferred_reap[cpu_id];
    g_deferred_reap[cpu_id] = NULL;
    process_t* prev = NULL;
    process_t* curr = proc_list_head;

    while (curr) {
        if (curr->state == PROCESS_ZOMBIE || curr->state == PROCESS_TERMINATED) {
            bool deferred = curr == deferred_reap;
            for (uint32_t i = 0; i < cpu_id && !deferred; i++) {
                deferred = curr == g_deferred_reap[i];
            }
            for (uint32_t i = cpu_id + 1; i < MAX_CPUS && !deferred; i++) {
                deferred = curr == g_deferred_reap[i];
            }
            // An owner's address space outlives its threads: reap them first.
            if (deferred || curr->on_cpu || process_is_current_on_any_cpu(curr) ||
                (!curr->mem_owner && curr->thread_count)) {
                prev = curr;
                curr = curr->next;
                continue;
            }
            if (curr->mem_owner && curr->mem_owner->thread_count)
                curr->mem_owner->thread_count--;

            if (SCHED_DEBUG >= 1) {
                serial_write_str("Cleaning: ");
                serial_write_str(curr->name);
                serial_write_str("\n");
            }

            process_t* to_free = curr;
            wm_watchdog_forget(to_free);

            if (prev) {
                prev->next = curr->next;
            } else {
                proc_list_head = curr->next;
            }

            // Killed while sleeping in a wait queue: its queue entry lives on
            // its kernel stack, so unlink it before that stack is freed.
            waitq_forget(to_free);

            if (to_free->kernel_stack) {
                void* stack_base = (void*)((uint8_t*)to_free->kernel_stack - KERNEL_STACK_SIZE);
                if (to_free->kernel_stack_page_backed)
                    free_pages(stack_base, to_free->kernel_stack_pages);
                else
                    free_mem(stack_base);
            }

                    proc_user_heap_cleanup(to_free);

            if (to_free->privilege == PROC_PRIVILEGE_USER) {
                proc_destroy_address_space(to_free);
            }

            // Record its exit for SYS_WAIT and drop its working directory,
            // environment and output files (spawn.c; no disk I/O here).
            uint64_t parent = proc_spawn_reap(to_free);
            if (parent && wake_count < sizeof(wake_parents) / sizeof(wake_parents[0]))
                wake_parents[wake_count++] = parent;

            free_mem(to_free);

            curr = (prev) ? prev->next : proc_list_head;
            continue;
        }
        prev = curr;
        curr = curr->next;
    }
    scheduler_unlock(sched_flags);
    for (uint32_t i = 0; i < wake_count; i++) proc_spawn_wake_parent(wake_parents[i]);

    // ---- Per-CPU dispatch ----
    process_t* old = current_process;
    // Live on this CPU until context_switch() saves it (some processes,
    // like the boot one, started running without passing through it).
    if (old) old->on_cpu = 1;
    process_t* idle = ap_idle_for(cpu_id);

    uint64_t rq_flags = spinlock_acquire(&rq->lock);

    process_t* next = NULL;
    for (;;) {
        next = rq_pop_runnable(rq, old);
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
        // Taken for this CPU under the queue lock, the same lock kill()
        // changes the state under: a killed process is either still queued
        // (and removed) or already marked running and on a CPU, where the
        // kill finds and stops it.
        next->state = PROCESS_RUNNING;
        next->on_cpu = 1;
        break;
    }

    if (next && next == old) {
        // A wait-queue wake-up (scheduler_wake) queued the process that is
        // still running here, before it switched away. It keeps the CPU.
        spinlock_release(&rq->lock, rq_flags);
        old->state = PROCESS_RUNNING;
        current_process = old;
        return;
    }

    if (!next) {
        // Nothing else ready on this core this tick. Whether `old` can go
        // on is decided before the queue lock is released: once it is, a
        // wake-up from another CPU may make it READY and queue it, and
        // treating that as "still runnable here" would leave it running
        // and queued at once.
        bool old_runnable = old && (old->state == PROCESS_RUNNING ||
                                    old->state == PROCESS_READY);
        // A thread whose affinity no longer includes this CPU goes to one
        // it may use even though nothing else wants this one.
        bool leave = old_runnable && idle && old != idle &&
                     old->privilege == PROC_PRIVILEGE_USER &&
                     !scheduler_cpu_allowed(old, cpu_id);
        if (leave) { old->state = PROCESS_READY; old_runnable = false; }
        else if (old_runnable) old->state = PROCESS_RUNNING;
        spinlock_release(&rq->lock, rq_flags);

        if (old_runnable) {
            current_process = old;
            return;
        }

        idle_cycles++;
        if (idle && old && old != idle) {
            // `old` is sleeping or exiting and nothing else is ready:
            // wait in this core's idle loop until a timer tick brings
            // work, rather than returning into a sleeping process.
            if (old->state == PROCESS_ZOMBIE || old->state == PROCESS_TERMINATED) {
                g_deferred_reap[cpu_id] = old;
            }
            idle->state = PROCESS_RUNNING;
            current_process = idle;
            if (leave) requeue_elsewhere(old);
            switch_to(old, idle);
        }
        return;
    }

    // Requeue the outgoing process (it was never itself in the queue
    // while running) behind whatever's now at the front of its level.
    bool migrate_old = false;
    if (old && old->state == PROCESS_RUNNING && old != idle) {
        old->state = PROCESS_READY;
        if (old->privilege == PROC_PRIVILEGE_USER && !scheduler_cpu_allowed(old, cpu_id))
            migrate_old = true;             // affinity changed: another CPU's queue
        else
            rq_push(rq, old, old->sched_level);
    }
    spinlock_release(&rq->lock, rq_flags);
    if (migrate_old) requeue_elsewhere(old);

    if (old && (old->state == PROCESS_ZOMBIE || old->state == PROCESS_TERMINATED)) {
        g_deferred_reap[cpu_id] = old;
    }
    next->sched_ticks_used = 0;
    next->state = PROCESS_RUNNING;
    current_process = next;
    tss_entry_t* tss = gdt_get_tss(cpu_id);
    if (tss && next->kernel_stack) {
        tss->rsp0 = (uint64_t)(uintptr_t)next->kernel_stack;
    }
    // Fires on every user context switch (60+ times a second for mosWM),
    // so it is only worth the serial I/O when booted with --debug.
    if (kernel_boot_debug && next->privilege == PROC_PRIVILEGE_USER) {
        serial_write_str("SCHED: dispatch user pid=");
        serial_write_dec(next->pid);
        serial_write_str("\n");
    }

    if (!old && !smp_is_master_core_id(cpu_id)) {
        // First process on an AP. The code running here is the AP's
        // boot idle loop (ap_entry_c), which no process owns. Park it as
        // this core's idle context and really start `next`; otherwise `next` would
        // be marked running while the idle loop kept the CPU, so its
        // entry point never ran (this is how 'kernelaudio' got lost on
        // cpu1 and audio playback never refilled).
        idle->state = PROCESS_RUNNING;
        switch_to(idle, next);
        return;
    }

    if (!old) {
        // First process the boot core has run: the code running here
        // (the terminal) carries on as that process. main() creates a
        // throwaway "busy" process first for exactly this.
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

    switch_to(old, next);
}

// Interrupts stay off for all of schedule(): kernel processes call it
// with them on (from sleep()/yield), and a timer tick landing between
// "current_process = next" and the actual switch ran a nested schedule()
// that saved this CPU's context into the wrong process. That process
// later resumed on another one's stack (#GP on iretq, seconds after an
// app started on a second core). Each context keeps its own flags: the
// switch saves and restores RFLAGS, and new processes start with IF=1.
void schedule() {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    schedule_irqs_off();
    if (flags & 0x200) asm volatile("sti" ::: "memory");
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
    process_t* self = current_process;
    self->wake_time_ms = wake_time;
    // Never turn a process that another CPU has just killed back into a
    // waiting one: it would be woken again and run on.
    for (;;) {
        process_state_t st = __atomic_load_n(&self->state, __ATOMIC_SEQ_CST);
        if (st == PROCESS_ZOMBIE || st == PROCESS_TERMINATED) {
            self->wake_time_ms = 0;
            schedule();
            return;
        }
        if (__atomic_compare_exchange_n(&self->state, &st, PROCESS_WAITING, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }

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

static void expire_cleanup_requests(void) {
    uint64_t now = time_get_uptime_ms();
    for (;;) {
        // Found and marked dead under the list lock (other CPUs reap from it);
        // the cleanup calls that follow only need the pid.
        uint64_t pid = 0;
        bool is_current = false;
        const char* name = NULL;
        uint64_t flags = scheduler_lock();
        for (process_t* proc = proc_list_head; proc; proc = proc->next) {
            if (!proc->cleanup_requested ||
                proc->state == PROCESS_ZOMBIE || proc->state == PROCESS_TERMINATED ||
                now < proc->cleanup_deadline_ms) {
                continue;
            }
            proc->cleanup_requested = false;
            pid = proc->pid;
            name = proc->name;      // valid until it is reaped, which needs the next tick
            is_current = proc == current_process;
            scheduler_mark_dead(proc, PROCESS_ZOMBIE);
            break;
        }
        scheduler_unlock(flags);
        if (!pid) return;
        serial_write_str("[PROC] Cleanup timeout, terminating ");
        serial_write_str(name);
        serial_write_str("\n");
        window_manager_process_exit(pid);
        fgpu_process_exit(pid);
        audio_streams_on_process_exit(pid);
        graphics_release_screen_for_pid(pid);
        if (is_current) schedule();
    }
}

void scheduler_tick() {
    if (!scheduler_on) {
        return;
    }

    wake_sleeping_processes();
    expire_cleanup_requests();

    bool was_idle = current_process == NULL;
    if (was_idle) {
        schedule();
    }

    uint32_t cpu_id = smp_current_cpu_id();
    if (cpu_id < MAX_CPUS) {
        cpu_local_t* cpu = &g_cpus[cpu_id];
        bool busy = current_process && current_process->state == PROCESS_RUNNING &&
                    !scheduler_is_idle_process(current_process);
        __atomic_add_fetch(&cpu->usage_total_ticks, 1, __ATOMIC_RELAXED);
        if (busy) {
            __atomic_add_fetch(&cpu->usage_busy_ticks, 1, __ATOMIC_RELAXED);
        }
        cpu->usage_window_ticks++;
        if (busy) cpu->usage_window_busy_ticks++;
        if (cpu->usage_window_ticks >= 1000) {
            uint32_t percent = (cpu->usage_window_busy_ticks * 100) /
                               cpu->usage_window_ticks;
            __atomic_store_n(&cpu->usage_last_percent, percent,
                             __ATOMIC_RELAXED);
            cpu->usage_window_ticks = 0;
            cpu->usage_window_busy_ticks = 0;
        }
    }

    if (was_idle || !current_process) return;
    if (scheduler_is_idle_process(current_process)) {
        // Idle on an AP: pick up anything woken or migrated here. Also
        // when the process that last ran here has ended: its cleanup was
        // put off until this core had left its stack (g_deferred_reap),
        // and the cleanup pass is part of schedule(). Without this an
        // idle core never freed it, and its parent waited forever. Two
        // passes: the first only notes that the stack is free now.
        static uint8_t reap_passes[MAX_CPUS];
        if (g_deferred_reap[cpu_id]) reap_passes[cpu_id] = 2;
        if (rq_total(&g_runqueues[cpu_id]) > 0) {
            schedule();
        } else if (reap_passes[cpu_id]) {
            reap_passes[cpu_id]--;
            schedule();
        }
        return;
    }

    // Killed while running here (another CPU's kill): leave it now. Also a
    // user thread whose affinity moved elsewhere.
    if (current_process->state == PROCESS_ZOMBIE || current_process->state == PROCESS_TERMINATED ||
        (current_process->privilege == PROC_PRIVILEGE_USER &&
         !scheduler_cpu_allowed(current_process, cpu_id))) {
        schedule();
        return;
    }

    current_process->sched_ticks_used++;
    current_process->cpu_time_ms++;

    uint64_t my_tick = __sync_add_and_fetch(&g_global_tick_count, 1);

    if ((my_tick % SCHED_BOOST_INTERVAL_TICKS) == 0) {
        scheduler_priority_boost();
    }
    if ((my_tick % SCHED_REBALANCE_INTERVAL_TICKS) == 0) {
        scheduler_rebalance();
    }

    uint32_t level = current_process->sched_level;
    if (level >= SCHED_NUM_LEVELS) level = SCHED_NUM_LEVELS - 1; // defensive

    // A process queued at a better level than the one running gets the
    // core now, not when the current quantum runs out. Without this a
    // freshly woken sleeper (mosWM's 16 ms frame sleep, the USB and
    // terminal pollers) sat behind whatever CPU-bound process held the
    // core - in practice the boot thread spinning at the end of
    // kernel_main(), which lives at the background level with a 10-tick
    // quantum and so capped every sleeper at one run per 100 ms.
    if (cpu_id < MAX_CPUS) {
        percpu_runqueue_t* rq = &g_runqueues[cpu_id];
        uint64_t rq_flags = spinlock_acquire(&rq->lock);
        bool better_ready = false;
        for (uint32_t i = 0; i < level && !better_ready; i++) {
            better_ready = rq->count[i] != 0;
        }
        spinlock_release(&rq->lock, rq_flags);
        if (better_ready) {
            schedule();
            return;
        }
    }

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

// ===========================================
// Killing, kicking, affinity and priority (OS-3)
// ===========================================

// Tells every other CPU that is running `p` to look at its run queue now
// (scheduler_ipi() makes it leave a dead or misplaced process).
void scheduler_kick(process_t* p) {
    if (!p) return;
    uint32_t self = smp_current_cpu_id();
    uint32_t n = g_cpu_count < MAX_CPUS ? g_cpu_count : MAX_CPUS;
    for (uint32_t i = 0; i < n; i++)
        if (i != self && g_cpus[i].online && smp_current_process_for_cpu(i) == p)
            smp_send_resched(i);
}

// Sets ZOMBIE (or TERMINATED) and takes the process off its run queue as
// one step under that queue's lock. The dispatcher pops and starts
// processes under the same lock, so afterwards a dead process is either
// never started or already marked on a CPU (on_cpu), and the CPU running
// it is kicked.
void scheduler_mark_dead(process_t* p, process_state_t state) {
    if (!p) return;
    percpu_runqueue_t* rq;
    uint64_t flags;
    for (;;) {
        uint32_t cpu = p->sched_cpu < MAX_CPUS ? p->sched_cpu : 0;
        rq = &g_runqueues[cpu];
        flags = spinlock_acquire(&rq->lock);
        uint32_t again = p->sched_cpu < MAX_CPUS ? p->sched_cpu : 0;
        if (again == cpu) break;
        spinlock_release(&rq->lock, flags);     // moved to another queue meanwhile
    }
    if (p->state != PROCESS_TERMINATED) __atomic_store_n(&p->state, state, __ATOMIC_SEQ_CST);
    rq_remove_locked(rq, p);
    spinlock_release(&rq->lock, flags);
    scheduler_kick(p);
}

// The reschedule IPI (tlb.c): the CPU runs its scheduler when its current
// process is dead or no longer allowed here, or when it is idle and work
// has been queued for it.
void scheduler_ipi(void) {
    if (!scheduler_on) return;
    process_t* p = current_process;
    if (!p) return;
    uint32_t cpu = smp_current_cpu_id();
    if (p->state == PROCESS_ZOMBIE || p->state == PROCESS_TERMINATED) { schedule(); return; }
    if (scheduler_is_idle_process(p)) {
        if (cpu < MAX_CPUS && rq_total(&g_runqueues[cpu]) > 0) schedule();
        return;
    }
    if (p->privilege == PROC_PRIVILEGE_USER && !scheduler_cpu_allowed(p, cpu)) schedule();
}

int scheduler_set_affinity(process_t* p, uint64_t mask) {
    if (!p) return -1;
    p->affinity = mask;
    if (p == current_process) return 0;     // the caller reschedules itself
    // Queued on a CPU it may not use: move it. Running elsewhere: kick it.
    percpu_runqueue_t* rq;
    uint64_t flags;
    uint32_t cpu;
    for (;;) {
        cpu = p->sched_cpu < MAX_CPUS ? p->sched_cpu : 0;
        rq = &g_runqueues[cpu];
        flags = spinlock_acquire(&rq->lock);
        uint32_t again = p->sched_cpu < MAX_CPUS ? p->sched_cpu : 0;
        if (again == cpu) break;
        spinlock_release(&rq->lock, flags);
    }
    bool moved = false;
    if (!scheduler_cpu_allowed(p, cpu) && p->state == PROCESS_READY)
        moved = rq_remove_locked(rq, p);
    spinlock_release(&rq->lock, flags);
    if (moved) requeue_elsewhere(p);
    else scheduler_kick(p);
    return 0;
}

void scheduler_set_priority(process_t* p, uint32_t prio) {
    if (!p) return;
    if (prio >= SCHED_NUM_LEVELS) prio = SCHED_NUM_LEVELS - 1;
    p->sched_prio = prio;
    // Takes effect at once for a level above the new floor; a lower level
    // only rises at the next wake or boost.
    if (p->sched_level < prio) p->sched_level = prio;
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

void process_exit_with_status(uint8_t exit_code, bool crashed,
                              uint8_t crash_vector) {
    if (!current_process) {
        PANIC("process_exit() called with no current process");
    }

    // Threads (SYS_THREAD): the program ending ends its threads, and a
    // crash in any of them ends the whole program. Threads run on several
    // CPUs, so two of them can crash at once, or one while the owner exits:
    // the first claims the program's end; the others just stop (waiting for
    // each other to leave the CPU would never finish).
    {
        process_t* self = current_process;
        process_t* grp = proc_mem_owner(self);
        if (self->privilege == PROC_PRIVILEGE_USER && grp &&
            (self->mem_owner || self->thread_count) && (crashed || !self->mem_owner)) {
            if (__atomic_exchange_n(&grp->group_dying, 1u, __ATOMIC_SEQ_CST)) {
                scheduler_mark_dead(self, PROCESS_TERMINATED);
                schedule();
                for (;;) asm volatile("sti; hlt" ::: "memory");
            }
        }
    }
    if (crashed && current_process->mem_owner) {
        // The program's own exit status says it crashed (SYS_WAIT).
        process_t* owner = current_process->mem_owner;
        owner->exit_code = exit_code;
        owner->crashed = true;
        owner->crash_vector = crash_vector;
    }
    if (crashed) proc_kill_group(current_process);
    else if (!current_process->mem_owner) proc_kill_threads(current_process);

    current_process->exit_code = exit_code;
    current_process->crashed = crashed;
    current_process->crash_vector = crash_vector;
    // Still in the process's own context, so disk writes are fine: write
    // out buffered program output, and release a thread's exit futex.
    proc_spawn_on_exit(current_process);
    window_manager_process_exit(current_process->pid);
    fgpu_process_exit(current_process->pid);
    audio_streams_on_process_exit(current_process->pid);
    graphics_release_screen_for_pid(current_process->pid);

    serial_write_str("[PROC] Exiting process: ");
    serial_write_str(current_process->name);
    serial_write_str("\n");

    command_user_process_exited(current_process->pid, exit_code, crashed,
                                crash_vector);
    if (crashed && current_process->mem_owner)     /* the terminal waits on the program */
        command_user_process_exited(current_process->mem_owner->pid, exit_code, crashed,
                                    crash_vector);
    // Its memory goes back now (never used again: this does not return to
    // the program). Its threads are off the run queues by this point.
    if (!current_process->mem_owner && current_process->privilege == PROC_PRIVILEGE_USER) {
        // Threads on other CPUs were told to stop; none may still run in
        // the address space that is about to be unmapped.
        proc_wait_threads_off_cpu(current_process);
        uvm_release_all(current_process);
    }
    current_process->state = PROCESS_TERMINATED;
    schedule();

    // There may be no ready process on this CPU yet. Keep the terminated
    // context alive until a timer tick can switch away from its stack.
    for (;;) {
        asm volatile("sti; hlt" ::: "memory");
    }
}

void process_exit(void) {
    process_exit_with_status(0, false, 0);
}