#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

/*
 * SMP-safe spinlock.
 *
 * cli()/sti() alone (safeints.h) only ever affects the local CPU core.
 * Any structure that could genuinely be touched from more than one
 * physical core at once (once real SMP bring-up exists) needs an
 * atomic lock like this one, IN ADDITION to disabling local
 * interrupts — the cli() protects against this core's own IRQ handler
 * (e.g. the PIT tick calling schedule()) reentering the same critical
 * section; the atomic test-and-set protects against another core doing
 * the same thing at the same time.
 *
 * Safe and correct to use today with a single core online — it just
 * never contends.
 */
typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock_t* lock) {
    lock->locked = 0;
}

/* Disables local interrupts and spins until the lock is acquired.
 * Returns the saved RFLAGS so the matching spinlock_release() call can
 * correctly restore whatever the interrupt-enable state was before. */
static inline uint64_t spinlock_acquire(spinlock_t* lock) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        asm volatile("pause" ::: "memory");
    }
    return flags;
}

static inline void spinlock_release(spinlock_t* lock, uint64_t flags) {
    __sync_lock_release(&lock->locked);
    if (flags & (1ULL << 9)) {
        asm volatile("sti" ::: "memory");
    }
}

#endif // SPINLOCK_H