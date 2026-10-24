#include "x86_64/smp_par.h"
#include "x86_64/smp.h"
#include "x86_64/lapic.h"
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
static uint8_t g_stacks[MAX_CPUS][HELPER_STACK_SIZE] __attribute__((aligned(16)));

static bool can_help(uint32_t cpu, uint32_t self) {
    return cpu != self && !smp_is_master_core_id(cpu) && g_cpus[cpu].online;
}

uint32_t smp_par_workers(void) {
    uint32_t self = smp_current_cpu_id(), n = 1;
    for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS; i++)
        if (can_help(i, self)) n++;
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

void smp_par_run(smp_par_fn fn, void* ctx, uint32_t parts) {
    if (!parts) return;
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
    __atomic_store_n(&g_job.open, 1, __ATOMIC_SEQ_CST);

    uint32_t self = smp_current_cpu_id(), asked = 0;
    for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS && asked + 1 < parts; i++) {
        if (!can_help(i, self)) continue;
        lapic_send_fixed_ipi(g_cpus[i].lapic_id, SMP_PAR_VECTOR);
        asked++;
    }

    work(0);
    while (__atomic_load_n(&g_job.done, __ATOMIC_SEQ_CST) < parts) __asm__ volatile("pause");
    __atomic_store_n(&g_job.open, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_job.active, __ATOMIC_SEQ_CST)) __asm__ volatile("pause");
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
