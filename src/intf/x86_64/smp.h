#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/proc.h"

#define MAX_CPUS 16

typedef struct {
    uint32_t   cpu_id;      // logical index 0..MAX_CPUS-1
    uint32_t   lapic_id;    // hardware APIC ID (Phase 3)
    bool       online;      // true once this core has entered the scheduler
    process_t* current_process;
    process_t* idle_process;
} cpu_local_t;

extern cpu_local_t g_cpus[MAX_CPUS];

// Number of cores currently known to be running. Stays 1 until Phase 3
// (real AP bring-up) is implemented.
extern volatile uint32_t g_cpu_count;

// Logical id of the calling core.
//
// Phase 3 will replace this with a read of the Local APIC ID register
// (or a per-core %gs-relative variable set up during AP entry). Until
// then there is exactly one core: the BSP, always id 0.
uint32_t smp_current_cpu_id(void);

// Zero and reset per-CPU bookkeeping, mark the BSP online. Call once,
// very early in kernel_main(), before any process is created.
void smp_init_bsp(void);