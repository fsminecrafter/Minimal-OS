#pragma once
#include <stdint.h>

/*
 * Runs one job on several cores at once (used by the FGPU rasterizer).
 *
 * smp_par_run(fn, ctx, parts) calls fn(ctx, part, worker) once for every
 * part below `parts`. The calling core is worker 0; other cores join when
 * an IPI (SMP_PAR_VECTOR) reaches them and take the next free part, so a
 * core that is busy with interrupts off simply does not help and is
 * never waited for. Helpers run with interrupts off, on their own stack
 * and the kernel's page tables. The master core (PIT, keyboard, USB) is
 * never asked to help. fn must not sleep or take sleeping locks.
 */

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
