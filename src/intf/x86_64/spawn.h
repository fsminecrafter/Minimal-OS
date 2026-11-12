#ifndef SPAWN_H
#define SPAWN_H

/*
 * Starting and waiting for programs from programs (SYS_SPAWN, SYS_WAIT),
 * plus the per-program state that goes with it: working directory
 * (SYS_CHDIR/SYS_GETCWD), environment, and output redirected into files.
 * See the ABI notes in syscall.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/proc.h"
#include "x86_64/syscall.h"

// Output of one or more programs going into a file. Buffered in the kernel
// and appended to the file in chunks and when a writer exits.
typedef struct stdio_sink stdio_sink_t;

// ---- syscalls ----
uint64_t proc_spawn(const syscall_spawn_t* request);
uint64_t proc_wait_child(uint64_t pid, uint64_t flags, syscall_wait_t* out);
uint64_t proc_chdir(const char* path);
uint64_t proc_getcwd(char* out, uint64_t capacity);

// Resolves `input` against `proc`'s working directory (its program's own,
// or the terminal's) into a fully qualified path. `resolved` holds
// MINIMAFS_MAX_PATH bytes. Safe on any CPU.
bool proc_resolve_path(process_t* proc, const char* input, char* resolved);

// Writes program output (fd 1 or 2) to the program's sink. Returns -1 when
// that stream is not redirected (the caller prints to the terminal), 0 when
// `buf` is not the program's memory, 1 when written.
int proc_stdio_write(process_t* proc, int fd, const char* buf, uint64_t len);

// SYS_EXEC from a program: like `run`, but the child inherits the caller's
// working directory, environment and output, and the caller may SYS_WAIT
// for it. `path` is already normalized. Returns the pid or an error.
uint64_t proc_exec_inherit(const char* path, int extra_argc, const char** extra_argv);

// Drops a reference to a sink, writing out what is buffered when it was
// the last one. Process context only (may do disk I/O).
void stdio_sink_put(stdio_sink_t* sink);

// ---- scheduler hooks ----
// At exit, in the exiting process's context: writes out its buffered
// output, and for a thread clears and wakes its exit futex.
void proc_spawn_on_exit(process_t* proc);
// At reap (scheduler lock held, interrupts off, no disk I/O): records the
// exit for SYS_WAIT, releases the process's sinks, frees its working
// directory and environment. Returns the pid of a parent to wake, or 0.
uint64_t proc_spawn_reap(process_t* proc);
// Wakes a parent sleeping in SYS_WAIT (after the scheduler lock is gone).
void proc_spawn_wake_parent(uint64_t parent_pid);

#endif // SPAWN_H
