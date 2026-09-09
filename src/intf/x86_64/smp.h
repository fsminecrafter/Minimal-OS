#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/proc.h"
#include "x86_64/multiboot2parse.h"

#define MAX_CPUS 16

typedef struct {
    uint32_t   cpu_id;
    uint32_t   lapic_id;
    bool       online;
    process_t* current_process;
    process_t* idle_process;
} cpu_local_t;

extern cpu_local_t g_cpus[MAX_CPUS];
extern volatile uint32_t g_cpu_count;

uint32_t smp_current_cpu_id(void);
void smp_init_bsp(void);

// Brings up every AP reported by ACPI's MADT. Call after
// smp_init_bsp(), gdt_init(), idt_init(), and startroutine() (needs a
// running PIT for INIT/SIPI timing). Returns cores online afterwards
// (including BSP); returns 1 and leaves the kernel single-core if
// ACPI/LAPIC init fails.
uint32_t smp_start_aps(multiboot2_info_t* mb_info);

// Entry point for every AP once it reaches 64-bit long mode - see
// ap_trampoline.asm. Never returns.
void ap_entry_c(uint32_t cpu_id) __attribute__((__noreturn__));