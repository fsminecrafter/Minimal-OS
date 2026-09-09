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

// ===========================================
// MASTER CORE
// ===========================================
//
// The "master core" is the single core allowed to run every legacy,
// hardware-shared IRQ subsystem that has not yet been redistributed
// via real IOAPIC redirection entries: the PIT tick (and everything
// it drives - time_tick(), usb_poll(), audio_update()) and the PS/2
// keyboard IRQ. On a single-core system it's trivially the only core.
// On this kernel's current (un-reprogrammed 8259 PIC, IOAPIC entries
// left masked) setup it is ALWAYS the BSP, because the PIC's fixed
// delivery mode sends every legacy IRQ to whichever core's local APIC
// was the interrupt destination at boot - always the boot processor,
// never an AP. That's presently correct by accident of hardware
// wiring, not because any software here checks it.
//
// These subsystems share global, unlocked state (the AC97 ring
// buffer, the USB controller's TD/QH structures, PS/2 keyboard
// state) with no protection beyond "only one core ever touches it".
// Distributing them across cores is real future work - IOAPIC
// redirection entries per IRQ, plus per-subsystem locking or an
// explicit single-owner-core design for the shared hardware resources
// themselves (there's only one audio DMA buffer and one PS/2
// controller, so "run on every core" isn't even the right model for
// them). Until that work happens, SMP_MASTER_CPU_ID and the assertion
// below exist purely to fail loudly the moment that invariant is
// accidentally broken, instead of producing an intermittent,
// hard-to-reproduce corruption bug on real multi-core hardware.
#define SMP_MASTER_CPU_ID 0u

static inline bool smp_is_master_core_id(uint32_t cpu_id) {
    return cpu_id == SMP_MASTER_CPU_ID;
}

static inline bool smp_is_master_core(void) {
    return smp_is_master_core_id(smp_current_cpu_id());
}

// Panics with a clear message identifying `subsystem` if called from
// any core other than the master core. Cheap enough (one LAPIC ID
// read + a linear scan over online cores, same cost
// smp_current_cpu_id() already pays) to call unconditionally at the
// top of every legacy-shared-IRQ handler - see pit_irq_handler() in
// pit.c and idt_handler_keyboard() in idt.c for the intended call
// sites.
void smp_assert_master_core(const char* subsystem);