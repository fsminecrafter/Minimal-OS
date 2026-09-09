#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t thread_id_t;

/*
 * A "thread" in MinimalOS is just a process_t that shares the kernel's
 * page table with everyone else (see proc_create()'s pml4 assignment) -
 * there is currently no address-space isolation between processes at
 * all, so createProcess() and thread_create() are the same primitive.
 * This wrapper exists to make caller intent explicit and to give you
 * thread_join(), which createProcess() alone doesn't provide.
 */

// Create a new thread. Runs entry() on its own kernel stack, scheduled
// independently of the caller. Returns 0 on failure.
thread_id_t thread_create(const char* name, void (*entry)(void));

// Block the calling thread until `tid` has exited. Safe to call from
// any thread. Returns immediately if `tid` is already gone or invalid.
void thread_join(thread_id_t tid);

// Exit the calling thread. Never returns.
void thread_exit(void) __attribute__((noreturn));

// PID of the calling thread.
thread_id_t thread_self(void);