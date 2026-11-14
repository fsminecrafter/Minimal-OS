#pragma once
#include <stdint.h>

/*
 * Runs one job on several cores at once (used by the FGPU rasterizer).
 *
 * smp_par_run(fn, ctx, parts) calls fn(ctx, part, worker) once for every
 * part below `parts`. The calling core is worker 0; other cores join and
 * take the next free part. A part nobody took is done by the caller, so a
 * busy core is never waited for, only a part it already started.
 *
 * Helpers are preemptible kernel processes "smp-par", one per core,
 * started by the first job run with interrupts on. They are woken for a
 * job, so they start at once on an idle core and at the next tick on a
 * busy one, and the scheduler shares the core between them and programs.
 * With the 'smpparipi' boot flag (and before the processes exist) helpers
 * run in an IPI handler (SMP_PAR_VECTOR) instead, with interrupts off, on
 * their own stack, holding up what that core runs until the job ends. The
 * master core (PIT, keyboard, USB) never helps. fn must not sleep or take
 * sleeping locks.
 */

#include <stdbool.h>
extern bool kernel_boot_smpparipi;

#define SMP_PAR_VECTOR 0x33
#define SMP_PAR_MAX_WORKERS 16u     // worker indices are below this

typedef void (*smp_par_fn)(void* ctx, uint32_t part, uint32_t worker);

// How many cores a job started on this core can use (1 = only this one).
uint32_t smp_par_workers(void);

// Returns when every part has finished. Runs everything on this core when
// no other core is online or another job is running.
void smp_par_run(smp_par_fn fn, void* ctx, uint32_t parts);

// The IPI handler (idt_.asm wraps it).
void smp_par_ipi_handler(void);
