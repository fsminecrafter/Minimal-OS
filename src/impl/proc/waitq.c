// src/impl/proc/waitq.c - kernel wait queues and SYS_FUTEX (see waitq.h)
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/waitq.h"
#include "x86_64/clock.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/spinlock.h"
#include "x86_64/tlb.h"
#include "x86_64/syscall.h"
#include "time.h"

#define WAITQ_BUCKETS 64u    // power of two

typedef struct {
    spinlock_t lock;
    waitq_waiter_t* head;
} waitq_bucket_t;

static waitq_bucket_t g_buckets[WAITQ_BUCKETS];

static waitq_bucket_t* waitq_bucket(const void* space, uint64_t key) {
    uint64_t h = (uint64_t)(uintptr_t)space * 0x9E3779B97F4A7C15ull;
    h ^= key * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return &g_buckets[h & (WAITQ_BUCKETS - 1u)];
}

static void waitq_unlink_locked(waitq_bucket_t* b, waitq_waiter_t* w) {
    waitq_waiter_t** link = &b->head;
    while (*link) {
        if (*link == w) {
            *link = w->next;
            w->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

// The bucket lock without touching the interrupt flag (the caller keeps
// interrupts off across schedule()).
static inline void waitq_raw_lock(waitq_bucket_t* b) {
    while (__sync_lock_test_and_set(&b->lock.locked, 1)) {
        tlb_poll();
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void waitq_raw_unlock(waitq_bucket_t* b) {
    __sync_lock_release(&b->lock.locked);
}

waitq_result_t waitq_wait(const void* space, uint64_t key,
                          waitq_should_sleep_fn should_sleep, void* ctx,
                          uint64_t deadline_ms) {
    process_t* self = current_process;
    if (!self || !should_sleep) return WAITQ_NOT_SLEPT;

    waitq_bucket_t* b = waitq_bucket(space, key);
    waitq_waiter_t w;
    w.next = NULL;
    w.proc = self;
    w.space = space;
    w.key = key;
    w.woken = 0;

    uint64_t flags = spinlock_acquire(&b->lock);
    if (!should_sleep(ctx)) {
        spinlock_release(&b->lock, flags);
        return WAITQ_NOT_SLEPT;
    }
    w.next = b->head;
    b->head = &w;
    self->waitq = &w;

    waitq_result_t result;
    for (;;) {
        if (w.woken) { result = WAITQ_WOKEN; break; }
        if (deadline_ms && time_get_uptime_ms() >= deadline_ms) {
            waitq_unlink_locked(b, &w);
            result = WAITQ_TIMEOUT;
            break;
        }

        self->wake_time_ms = deadline_ms;
        // RUNNING -> WAITING. Anything else means another CPU killed it
        // (ZOMBIE) meanwhile: a plain store would bring it back to life.
        // Let the syscall return; the scheduler takes it off the CPU.
        process_state_t running = PROCESS_RUNNING;
        if (!__atomic_compare_exchange_n(&self->state, &running, PROCESS_WAITING,
                                         false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            self->wake_time_ms = 0;
            waitq_unlink_locked(b, &w);
            result = WAITQ_TIMEOUT;
            break;
        }
        // From here a waker can make it READY; interrupts stay off until
        // schedule() has switched away (or found nothing else to run).
        waitq_raw_unlock(b);
        schedule();
        if (self->state == PROCESS_WAITING && !w.woken) {
            // schedule() came straight back: the boot core has no idle
            // context, so wait here for an interrupt (a tick, a wake).
            __asm__ volatile("sti; hlt; cli" ::: "memory");
        }
        scheduler_resume_self();
        waitq_raw_lock(b);
    }

    self->waitq = NULL;
    self->wake_time_ms = 0;
    spinlock_release(&b->lock, flags);
    return result;
}

uint32_t waitq_wake(const void* space, uint64_t key, uint32_t max) {
    if (!max) return 0;
    waitq_bucket_t* b = waitq_bucket(space, key);
    uint32_t woken = 0;

    uint64_t flags = spinlock_acquire(&b->lock);
    waitq_waiter_t** link = &b->head;
    while (*link && woken < max) {
        waitq_waiter_t* w = *link;
        if (w->space != space || w->key != key) {
            link = &w->next;
            continue;
        }
        *link = w->next;
        w->next = NULL;
        process_t* p = w->proc;
        __atomic_store_n(&w->woken, 1u, __ATOMIC_SEQ_CST);
        scheduler_wake(p);      // no-op unless it is really asleep
        woken++;
    }
    spinlock_release(&b->lock, flags);
    return woken;
}

void waitq_forget(process_t* proc) {
    if (!proc || !proc->waitq) return;
    waitq_waiter_t* w = proc->waitq;
    waitq_bucket_t* b = waitq_bucket(w->space, w->key);
    uint64_t flags = spinlock_acquire(&b->lock);
    waitq_unlink_locked(b, w);
    spinlock_release(&b->lock, flags);
    proc->waitq = NULL;
}

/* ------------------------------------------------------------------ futex */

typedef struct {
    volatile uint32_t* addr;
    uint32_t expected;
} futex_ctx_t;

static bool futex_should_sleep(void* ctx) {
    futex_ctx_t* f = (futex_ctx_t*)ctx;
    return __atomic_load_n(f->addr, __ATOMIC_SEQ_CST) == f->expected;
}

static bool futex_addr_ok(process_t* owner, const uint32_t* addr) {
    if (!owner || !addr || ((uintptr_t)addr & 3u)) return false;
    return proc_user_range_contains(owner, addr, sizeof(uint32_t)) != 0;
}

uint64_t futex_wait(uint32_t* addr, uint32_t expected, uint64_t timeout_ns) {
    process_t* owner = proc_mem_owner(current_process);
    if (!owner || owner->privilege != PROC_PRIVILEGE_USER) return SYS_ERR_PERM;
    if (!futex_addr_ok(owner, addr)) return SYS_ERR_INVAL;

    uint64_t deadline = 0;
    if (timeout_ns) {
        // Whole milliseconds, rounded up, plus one: the millisecond that is
        // running now is already partly over, and a timed wait must never
        // return early.
        uint64_t ms = timeout_ns / 1000000u + ((timeout_ns % 1000000u) ? 1u : 0u);
        deadline = time_get_uptime_ms() + ms + 1u;
    }

    futex_ctx_t ctx = { addr, expected };
    uint64_t start_ns = timeout_ns ? clock_monotonic_ns() : 0;
    waitq_result_t result = waitq_wait(owner, (uint64_t)(uintptr_t)addr, futex_should_sleep,
                                       &ctx, deadline);
    if (result == WAITQ_NOT_SLEPT) return SYS_ERR_AGAIN;
    // The deadline counts timer ticks, the program measures with the
    // monotonic clock, and the two drift (ticks can arrive in bursts in a
    // virtual machine). Time out only once the monotonic clock agrees.
    while (result == WAITQ_TIMEOUT && clock_monotonic_ns() - start_ns < timeout_ns) {
        result = waitq_wait(owner, (uint64_t)(uintptr_t)addr, futex_should_sleep, &ctx,
                            time_get_uptime_ms() + 1u);
        // The word changed while we were not queued: as good as a wake-up.
        if (result == WAITQ_NOT_SLEPT) return SYS_SUCCESS;
    }
    return result == WAITQ_TIMEOUT ? SYS_ERR_TIMEDOUT : SYS_SUCCESS;
}

uint64_t futex_wake(uint32_t* addr, uint32_t max) {
    process_t* owner = proc_mem_owner(current_process);
    if (!owner || owner->privilege != PROC_PRIVILEGE_USER) return SYS_ERR_PERM;
    if (!futex_addr_ok(owner, addr)) return SYS_ERR_INVAL;
    return waitq_wake(owner, (uint64_t)(uintptr_t)addr, max);
}

uint32_t futex_wake_for(process_t* owner, uint64_t addr, uint32_t max) {
    if (!owner) return 0;
    return waitq_wake(owner, addr, max);
}
