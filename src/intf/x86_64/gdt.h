#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include "x86_64/tss.h"

#define GDT_SELECTOR_NULL      0x00
#define GDT_SELECTOR_CS_KERNEL 0x08   // unchanged - idt.c already builds gates against this
#define GDT_SELECTOR_DS_KERNEL 0x10

// One 16-byte TSS descriptor per core, right after the fixed
// null/code/data entries. Replaces the old fixed
// _TSS/_TSS_LOW/_TSS_HIGH constants, which predate there being more
// than one core to have a TSS each.
#define GDT_TSS_SELECTOR(cpu) (0x18 + (cpu) * 16)
#define GDT_SELECTOR_TSS      GDT_TSS_SELECTOR(0)

#define IDT_GATE_TYPE_INTERRUPT 0x0E
#define IDT_GATE_TYPE_TRAP      0x0F
#define IDT_GATE_PRESENT        0x80
#define IDT_GATE_DPL0           0x00
#define IDT_GATE_DPL3           0x60

#define IDT_INTERRUPT_GATE_FLAGS (IDT_GATE_PRESENT | IDT_GATE_DPL0 | IDT_GATE_TYPE_INTERRUPT)

// Builds the real GDT (previously just the 2-entry stub in main.asm)
// plus one TSS per MAX_CPUS core, and loads it on the BSP. Call once,
// early in kernel_main(), BEFORE idt_init() - the double-fault gate
// idt_init() installs uses IST1, which requires a valid TSS already
// loaded via ltr() or it's undefined behaviour the moment a double
// fault actually happens.
void gdt_init(void);

// Loads the shared GDT and this core's own TSS selector. Called by
// every AP right after reaching long mode (see ap_entry_c()).
void gdt_load_this_cpu(uint32_t cpu_id);

tss_entry_t* gdt_get_tss(uint32_t cpu_id);

void ltr(uint16_t sel);

#endif