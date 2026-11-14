// src/impl/kernel/tlb.c - TLB shootdown and the reschedule IPI (see tlb.h)
#include "x86_64/tlb.h"
#include "x86_64/smp.h"
#include "x86_64/lapic.h"
#include "serial.h"
#include "time.h"

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
// Above this many pages a CR3 reload is cheaper than invlpg per page.
#define TLB_RANGE_MAX_PAGES 48

volatile uint32_t g_tlb_pending;

static struct {
    uint64_t cr3;
    uint64_t start, end;
} g_req;
static volatile uint32_t g_tlb_lock;
static volatile uint8_t g_in_service[MAX_CPUS];

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore_flags(uint64_t flags) {
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
}

// Flushes this CPU's TLB for the request, if it has the space loaded.
static void flush_local(uint64_t space_cr3, uint64_t start, uint64_t end) {
    uint64_t cr3 = read_cr3();
    if ((cr3 & PTE_ADDR_MASK) != space_cr3) return;
    if (start == end || (end - start) / 4096 > TLB_RANGE_MAX_PAGES) {
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
        return;
    }
    for (uint64_t a = start & ~0xFFFULL; a < end; a += 4096)
        __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
}

void tlb_service(void) {
    uint64_t flags = irq_save();     // the CPU id and the flush must stay together
    uint32_t cpu = smp_current_cpu_id();
    uint32_t bit = 1u << cpu;
    if (cpu < MAX_CPUS && (__atomic_load_n(&g_tlb_pending, __ATOMIC_ACQUIRE) & bit) &&
        !g_in_service[cpu]) {
        g_in_service[cpu] = 1;
        __asm__ volatile("" ::: "memory");
        flush_local(g_req.cr3, g_req.start, g_req.end);
        __atomic_fetch_and(&g_tlb_pending, ~bit, __ATOMIC_SEQ_CST);
        g_in_service[cpu] = 0;
    }
    irq_restore_flags(flags);
}

void tlb_ipi_handler(void) {
    lapic_send_eoi();
    tlb_service();
}

void tlb_shootdown(volatile uint32_t* loaded_cpus, uint64_t space_cr3,
                   uint64_t start, uint64_t end) {
    space_cr3 &= PTE_ADDR_MASK;
    uint64_t flags = irq_save();
    while (__atomic_exchange_n(&g_tlb_lock, 1, __ATOMIC_ACQUIRE)) {
        tlb_service();
        __asm__ volatile("pause" ::: "memory");
    }
    uint32_t self = smp_current_cpu_id();
    // The page-table writes come first, the mask read after: a CPU that
    // sets its bit later loads the new tables.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t mask = __atomic_load_n(loaded_cpus, __ATOMIC_SEQ_CST);
    uint32_t online = 0;
    for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS; i++)
        if (g_cpus[i].online) online |= 1u << i;
    mask &= online & ~(1u << self);

    flush_local(space_cr3, start, end);
    if (mask) {
        g_req.cr3 = space_cr3;
        g_req.start = start;
        g_req.end = end;
        __atomic_store_n(&g_tlb_pending, mask, __ATOMIC_SEQ_CST);
        for (uint32_t i = 0; i < MAX_CPUS; i++)
            if (mask & (1u << i)) lapic_send_fixed_ipi(g_cpus[i].lapic_id, TLB_IPI_VECTOR);
        uint64_t began = time_get_uptime_ms();
        bool warned = false;
        while (__atomic_load_n(&g_tlb_pending, __ATOMIC_ACQUIRE)) {
            tlb_service();
            __asm__ volatile("pause" ::: "memory");
            if (!warned && time_get_uptime_ms() - began > 3000) {
                warned = true;
                serial_write_str("[PROC] TLB shootdown: no acknowledgement from cpu mask 0x");
                serial_write_hex(g_tlb_pending);
                serial_write_str("\n");
            }
        }
    }
    __atomic_store_n(&g_tlb_lock, 0, __ATOMIC_RELEASE);
    irq_restore_flags(flags);
}

void resched_ipi_handler(void) {
    lapic_send_eoi();
    extern void scheduler_ipi(void);
    scheduler_ipi();
}

void smp_send_resched(uint32_t cpu) {
    if (cpu >= MAX_CPUS || !g_cpus[cpu].online) return;
    lapic_send_fixed_ipi(g_cpus[cpu].lapic_id, RESCHED_IPI_VECTOR);
}
