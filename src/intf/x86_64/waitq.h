#ifndef WAITQ_H
#define WAITQ_H

/*
 * Kernel wait queues: a process sleeps on a key until someone wakes that
 * key. SYS_FUTEX (key = the program's memory + the word's address) and
 * SYS_WAIT (key = the parent's pid) are built on it.
 *
 * Waiting checks its condition and goes to sleep under the key's bucket
 * lock, and waking takes the same lock, so a wake-up cannot slip in between
 * "the condition says sleep" and "asleep": no lost wake-ups.
 */

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/proc.h"

typedef struct waitq_waiter {
    struct waitq_waiter* next;
    process_t* proc;
    const void* space;          // what the key belongs to (a process, a tag)
    uint64_t key;
    volatile uint32_t woken;
} waitq_waiter_t;

typedef enum {
    WAITQ_WOKEN = 0,            // woken by waitq_wake() (or spuriously)
    WAITQ_TIMEOUT,              // the deadline passed
    WAITQ_NOT_SLEPT,            // should_sleep() said no; did not sleep
} waitq_result_t;

// Runs under the bucket lock with interrupts off: must not sleep or take a
// lock that is held while calling waitq_wake().
typedef bool (*waitq_should_sleep_fn)(void* ctx);

// Sleeps the calling process on (space, key) while should_sleep(ctx) holds,
// until waitq_wake() or deadline_ms (uptime in ms; 0 = none). Returns after
// one wake-up: callers re-check their condition and call again as needed.
waitq_result_t waitq_wait(const void* space, uint64_t key,
                          waitq_should_sleep_fn should_sleep, void* ctx,
                          uint64_t deadline_ms);

// Wakes up to `max` processes sleeping on (space, key). Returns how many.
uint32_t waitq_wake(const void* space, uint64_t key, uint32_t max);

// Takes a process that is being freed off any wait queue (scheduler reap).
void waitq_forget(process_t* proc);

// ---- futex (SYS_FUTEX) on top of it ----
uint64_t futex_wait(uint32_t* addr, uint32_t expected, uint64_t timeout_ns);
uint64_t futex_wake(uint32_t* addr, uint32_t max);
// Futex wake from the kernel on behalf of `owner`'s memory (thread exit).
uint32_t futex_wake_for(process_t* owner, uint64_t addr, uint32_t max);

#endif // WAITQ_H
