#include "x86_64/smp_par.h"
#include "x86_64/smp.h"
#include "x86_64/lapic.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/waitq.h"
#include "x86_64/tlb.h"
#include "serial.h"
#include <stdbool.h>

#define HELPER_STACK_SIZE 0x4000

extern uint64_t pml4_phys_addr;
// Calls fn(arg) on another stack (smp_par_.asm).
extern void smp_par_call_on_stack(void (*fn)(uint32_t), uint32_t arg, void* stack_top);

static struct {
    smp_par_fn fn;
    void* ctx;
    uint32_t parts;
    volatile uint32_t open;         // helpers may join
    volatile uint32_t next;         // next part to hand out
    volatile uint32_t done;         // parts finished
    volatile uint32_t active;       // helpers inside the handler
    volatile uint32_t next_worker;
} g_job;

static volatile uint32_t g_lock;

/* Helpers are preemptible kernel processes, one per core (OS-4), unless
 * the 'smpparipi' boot flag asks for the old way: helpers inside an IPI
 * handler with interrupts off, which hold up whatever that core runs until
 * the job ends. The IPI way is also used until the processes are up. */
bool kernel_boot_smpparipi;
static volatile uint32_t g_gen;                 // bumped for every job
static volatile uint32_t g_threads_state;       // 0 none, 1 starting, 2 started
static volatile uint32_t g_spawn_next;
static uint32_t g_spawn_cpu[MAX_CPUS];
static volatile uint8_t g_thread_on[MAX_CPUS];  // a helper process serves this core
static uint8_t g_stacks[MAX_CPUS][HELPER_STACK_SIZE] __attribute__((aligned(16)));

static bool can_help(uint32_t cpu, uint32_t self) {
    return cpu != self && !smp_is_master_core_id(cpu) && g_cpus[cpu].online;
}

static bool use_threads(void) {
    return !kernel_boot_smpparipi && __atomic_load_n(&g_threads_state, __ATOMIC_ACQUIRE) == 2;
}

static bool helper_for(uint32_t cpu, uint32_t self) {
    if (!can_help(cpu, self)) return false;
    return !use_threads() || g_thread_on[cpu];
}

uint32_t smp_par_workers(void) {
    uint32_t self = smp_current_cpu_id(), n = 1;
    for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS; i++)
        if (helper_for(i, self)) n++;
    return n < SMP_PAR_MAX_WORKERS ? n : SMP_PAR_MAX_WORKERS;
}

static void work(uint32_t worker) {
    for (;;) {
        uint32_t part = __atomic_fetch_add(&g_job.next, 1, __ATOMIC_SEQ_CST);
        if (part >= g_job.parts) return;
        g_job.fn(g_job.ctx, part, worker);
        __atomic_fetch_add(&g_job.done, 1, __ATOMIC_SEQ_CST);
    }
}

// Joins the current job if it is still the one this helper was woken for.
static void join(uint32_t gen) {
    __atomic_fetch_add(&g_job.active, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_job.open, __ATOMIC_SEQ_CST) &&
        __atomic_load_n(&g_gen, __ATOMIC_SEQ_CST) == gen) {
        uint32_t worker = __atomic_fetch_add(&g_job.next_worker, 1, __ATOMIC_SEQ_CST);
        if (worker < SMP_PAR_MAX_WORKERS) work(worker);
    }
    __atomic_fetch_sub(&g_job.active, 1, __ATOMIC_SEQ_CST);
}

static bool helper_should_sleep(void* p) {
    uint32_t seen = *(const uint32_t*)p;
    return !(__atomic_load_n(&g_job.open, __ATOMIC_SEQ_CST) &&
             __atomic_load_n(&g_gen, __ATOMIC_SEQ_CST) != seen);
}

// A helper process: pinned to its core, asleep until a job starts.
static void helper_main(void) {
    uint32_t idx = __atomic_fetch_add(&g_spawn_next, 1, __ATOMIC_SEQ_CST);
    uint32_t cpu = g_spawn_cpu[idx];
    process_t* self = current_process;
    scheduler_set_affinity(self, 1ull << cpu);
    scheduler_set_priority(self, SCHED_LEVEL_INTERACTIVE);
    // Move to that core (a sleep goes through the scheduler).
    while (smp_current_cpu_id() != cpu) sleep(1);
    g_thread_on[cpu] = 1;
    __asm__ volatile("sti");
    uint32_t seen = __atomic_load_n(&g_gen, __ATOMIC_SEQ_CST);
    for (;;) {
        waitq_wait(&g_job, 0, helper_should_sleep, &seen, 0);
        uint32_t gen = __atomic_load_n(&g_gen, __ATOMIC_SEQ_CST);
        if (gen == seen) continue;
        seen = gen;
        join(gen);
    }
}

// Starts one helper process per core that can help. Only from a context
// that may allocate and has interrupts on.
static void start_threads(void) {
    uint32_t expect = 0;
    if (!__atomic_compare_exchange_n(&g_threads_state, &expect, 1, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS; i++)
        if (!smp_is_master_core_id(i) && g_cpus[i].online) g_spawn_cpu[n++] = i;
    for (uint32_t k = 0; k < n; k++) proc_create("smp-par", helper_main);
    __atomic_store_n(&g_threads_state, 2, __ATOMIC_RELEASE);
    serial_write_str("smp_par: helper processes started: ");
    serial_write_hex(n);
    serial_write_str("\n");
}

void smp_par_run(smp_par_fn fn, void* ctx, uint32_t parts) {
    if (!parts) return;
    if (!kernel_boot_smpparipi && !g_threads_state) {
        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
        if (rflags & (1ull << 9)) start_threads();
    }
    if (parts == 1 || smp_par_workers() <= 1 || __atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
        for (uint32_t p = 0; p < parts; p++) fn(ctx, p, 0);
        return;
    }
    // No helper is inside (the last job waited for that), so the fields
    // can be set freely.
    g_job.fn = fn;
    g_job.ctx = ctx;
    g_job.parts = parts;
    g_job.next = 0;
    g_job.done = 0;
    g_job.next_worker = 1;
    bool threads = use_threads();
    __atomic_fetch_add(&g_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_job.open, 1, __ATOMIC_SEQ_CST);

    uint32_t self = smp_current_cpu_id();
    if (threads) {
        // Helpers on idle cores start at once (the wake-up IPIs them);
        // on busy ones at their next tick, and the job does not wait for
        // a part nobody took.
        waitq_wake(&g_job, 0, 0xFFFFFFFFu);
    } else {
        uint32_t asked = 0;
        for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS && asked + 1 < parts; i++) {
            if (!can_help(i, self)) continue;
            lapic_send_fixed_ipi(g_cpus[i].lapic_id, SMP_PAR_VECTOR);
            asked++;
        }
    }

    work(0);
    // A helper may be in the middle of a part, preempted on its core: wait
    // for it (taking part in TLB shootdowns meanwhile if interrupts are off).
    while (__atomic_load_n(&g_job.done, __ATOMIC_SEQ_CST) < parts) { tlb_poll(); __asm__ volatile("pause"); }
    __atomic_store_n(&g_job.open, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_job.active, __ATOMIC_SEQ_CST)) { tlb_poll(); __asm__ volatile("pause"); }
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

void smp_par_ipi_handler(void) {
    lapic_send_eoi();
    __atomic_fetch_add(&g_job.active, 1, __ATOMIC_SEQ_CST);
    // A late IPI (the job already ended) finds the job closed.
    if (__atomic_load_n(&g_job.open, __ATOMIC_SEQ_CST)) {
        uint32_t worker = __atomic_fetch_add(&g_job.next_worker, 1, __ATOMIC_SEQ_CST);
        uint32_t cpu = smp_current_cpu_id();
        if (worker < SMP_PAR_MAX_WORKERS && cpu < MAX_CPUS) {
            // This core may be running a program whose page tables hide
            // some kernel memory behind its own mappings; the job's data
            // is all visible in the kernel's.
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            bool swap = (cr3 & ~0xFFFull) != pml4_phys_addr;
            if (swap) __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys_addr) : "memory");
            smp_par_call_on_stack(work, worker, g_stacks[cpu] + HELPER_STACK_SIZE);
            if (swap) __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
        }
    }
    __atomic_fetch_sub(&g_job.active, 1, __ATOMIC_SEQ_CST);
}
