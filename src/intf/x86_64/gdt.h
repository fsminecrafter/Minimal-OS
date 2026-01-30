#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include "x86_64/tss.h"

extern struct tss_struct_t tss_struct;

#define GDT_SELECTOR_TSS          0x28    // entry 5 << 3, TSS descriptor spans 5 and 6
#define TSS_SELECTOR              GDT_SELECTOR_TSS
// GDT selectors (offsets into the GDT table, shifted left by 3 bits for the selector)
#define GDT_SELECTOR_NULL         0x00  // Null descriptor (mandatory first entry)
#define GDT_SELECTOR_CS_KERNEL    0x08  // Kernel code segment selector (2nd entry, index=1)
#define GDT_SELECTOR_DS_KERNEL    0x10  // Kernel data segment selector (3rd entry, index=2)
#define GDT_SELECTOR_TSS_LOW      0x18  // TSS descriptor low part (4th entry, index=3)
#define GDT_SELECTOR_TSS_HIGH     0x20  // TSS descriptor high part (5th entry, index=4)

// IDT gate types and flags for interrupt gates
#define IDT_GATE_TYPE_INTERRUPT   0x0E  // Interrupt Gate type (32-bit or 64-bit)
#define IDT_GATE_TYPE_TRAP        0x0F  // Trap Gate type
#define IDT_GATE_PRESENT          0x80  // Present flag
#define IDT_GATE_DPL0             0x00  // Descriptor privilege level 0 (kernel)
#define IDT_GATE_DPL3             0x60  // Descriptor privilege level 3 (user)

// Compose flags for a typical interrupt gate
#define IDT_INTERRUPT_GATE_FLAGS  (IDT_GATE_PRESENT | IDT_GATE_DPL0 | IDT_GATE_TYPE_INTERRUPT)


void gdt_init(void);
void ltr(uint16_t sel);

#endif
