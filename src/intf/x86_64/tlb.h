#ifndef TLB_H
#define TLB_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Cross-CPU TLB shootdown and the reschedule IPI (OS-3: threads of one
 * program run on several CPUs at once).
 *
 * A change to a shared address space that removes or reduces access must
 * be flushed on every CPU that may have the space loaded before the
 * frames behind it are reused and before the call returns:
 *
 *     tlb_shootdown(&space->loaded_cpus, space_pml4, start, end)
 *
 * `loaded_cpus` is the space's CPU mask: scheduler.c sets a CPU's bit
 * (uvm_cpu_switch) before it loads the space's CR3 and clears it after it
 * has left it, always with interrupts off. The sender flushes its own TLB,
 * posts one request (a single global slot, serialized by a lock), sends
 * TLB_IPI_VECTOR to the CPUs in the mask and waits until every one of
 * them has flushed and acknowledged.
 *
 * Deadlock avoidance: a CPU that waits (for the request slot, for acks, or
 * on any kernel spinlock: spinlock_acquire() and the space lock poll)
 * services a pending request for itself while it spins, so two CPUs
 * shooting at each other cannot wait forever. A CPU that runs with
 * interrupts off for a long time and never polls (a long system call)
 * delays the acknowledgement until it does; that is the grace period that
 * keeps frames from being freed under a system call that is using them.
 *
 * Never call tlb_shootdown() holding a lock that a spinning CPU would not
 * poll for (every spinlock in the kernel does, via spinlock_acquire()).
 */

#define TLB_IPI_VECTOR     0x34
#define RESCHED_IPI_VECTOR 0x35

// Bit per CPU that still has to acknowledge the current request.
extern volatile uint32_t g_tlb_pending;

// Handles the request for this CPU, if there is one. Safe anywhere.
void tlb_service(void);

static inline void tlb_poll(void) {
    if (__atomic_load_n(&g_tlb_pending, __ATOMIC_RELAXED)) tlb_service();
}

// start == end: flush everything of the space (also the paging-structure
// caches, which must happen before page-table pages are freed).
void tlb_shootdown(volatile uint32_t* loaded_cpus, uint64_t space_cr3,
                   uint64_t start, uint64_t end);

void tlb_ipi_handler(void);
void resched_ipi_handler(void);

// Asks a CPU to run its scheduler (scheduler_ipi() in scheduler.c).
void smp_send_resched(uint32_t cpu);

#endif
