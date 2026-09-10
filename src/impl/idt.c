#include <stddef.h>
#include <stdint.h>
#include "x86_64/gdt.h"
#include "x86_64/idt.h"
#include "x86_64/pic.h"
#include "x86_64/scheduler.h"
#include "panic.h"
#include "print.h"
#include "x86_64/exec_trace.h"
#include "x86_64/lapic.h"
#include "x86_64/lapic_timer.h"
#include "x86_64/smp.h"

#define IDT_IRQ0_TIMER 0x20
#define IDT_IRQ1_KEYBOARD 0x21

#define IDT_TYPE_INTERRUPT_GATE  0x0E
#define IDT_TYPE_TRAP_GATE       0x0F

// Flags for IDT entry
#define IDT_PRESENT              0x80
#define IDT_DPL0                 0x00
#define IDT_DPL3                 0x60

// Compose IDT entry type_attr byte for a kernel interrupt gate
#define IDT_ENTRY_TYPE_INTERRUPT (IDT_PRESENT | IDT_DPL0 | IDT_TYPE_INTERRUPT_GATE)
#define IDT_ENTRY_TYPE_TRAP      (IDT_PRESENT | IDT_DPL0 | IDT_TYPE_TRAP_GATE)

void (*idt_handler_pit_user)() = NULL;
extern void idt_handler_pit_wrapped();
extern void idt_handler_doublefault_wrapped();
extern void isr_spurious();
extern void lapic_timer_irq_handler_wrapped();

extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
extern void isr9();  extern void isr10(); extern void isr11(); extern void isr12();
extern void isr13(); extern void isr14(); extern void isr15(); extern void isr16();
extern void isr17(); extern void isr18(); extern void isr19(); extern void isr20();
extern void isr21(); extern void isr22(); extern void isr23(); extern void isr24();
extern void isr25(); extern void isr26(); extern void isr27(); extern void isr28();
extern void isr29(); extern void isr30(); extern void isr31();

static void (*const isr_stub_table[32])() = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    NULL /* 8: keeps idt_handler_doublefault_wrapped, installed separately */,
    isr9,  isr10, isr11, isr12, isr13, isr14, isr15, isr16,
    isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24,
    isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

extern void isr_syscall_wrapped();

struct IdtEntry {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t  ist: 3;
	uint8_t zero: 5;
	uint8_t  type;
	uint16_t offset_mid;
	uint32_t offset_high;
	uint32_t reserved;
} __attribute__((packed));

struct IdtEntry idt[256] __attribute__((aligned(16)));
struct IdtPtr idt_ptr;

void (*idt_handler_keyboard_user)();

extern void idt_handler_keyboard_wrapped();


void idt_reload() {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    idt_load(&idt_ptr);
}

void idt_handler_keyboard() {
	smp_assert_master_core("PS/2 keyboard IRQ1");

	if (idt_handler_keyboard_user != NULL) {
		idt_handler_keyboard_user();
	}
	
	pic_eoi_master();
}


void idt_set_entry(uint8_t vector,uint64_t isr_addr,uint16_t selector,uint8_t type_attr,uint8_t ist_index)
{
    idt[vector] = (struct IdtEntry){
      .offset_low  = (uint16_t)(isr_addr & 0xFFFF),
      .selector    = selector,
      .ist         = ist_index & 0x7,
      .zero        = 0,
      .type        = type_attr,
      .offset_mid  = (uint16_t)((isr_addr >> 16) & 0xFFFF),
      .offset_high = (uint32_t)((isr_addr >> 32) & 0xFFFFFFFF),
      .reserved    = 0,
    };
}

void idt_init() {
	pic_remap();
	
	idt_ptr.limit = (sizeof(struct IdtEntry) * 256) - 1;
	idt_ptr.base = (uint64_t) &idt;

	// General CPU exceptions (0-31), except 8 (double fault, handled below
	// with its own IST1 stack). Previously left as zeroed/absent gates -
	// any of these firing was an immediate triple fault with no diagnostic.
	for (int v = 0; v < 32; v++) {
		if (v == 8 || !isr_stub_table[v]) continue;
		idt_set_entry(v, (uint64_t)isr_stub_table[v], GDT_SELECTOR_CS_KERNEL,
		              IDT_ENTRY_TYPE_INTERRUPT, 0);
	}

	idt_set_entry(8, (uint64_t)idt_handler_doublefault_wrapped, GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 1);
	idt_set_entry(IDT_IRQ1_KEYBOARD, (uint64_t) idt_handler_keyboard_wrapped, GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 0);
	idt_set_entry(IDT_IRQ0_TIMER,    (uint64_t) idt_handler_pit_wrapped,      GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 0);

	// Spurious LAPIC vector - see isr_spurious in idt_isr.asm for why it
	// must never EOI. Only matters once LAPIC is in use (SMP), but install
	// it unconditionally so it's never a null gate.
	idt_set_entry(LAPIC_SPURIOUS_VECTOR, (uint64_t)isr_spurious, GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 0);

	// Per-core scheduler tick, driven by each core's own LAPIC timer.
	// See lapic_timer_init() / ap_entry_c() in smp.c.
	idt_set_entry(LAPIC_TIMER_VECTOR, (uint64_t)lapic_timer_irq_handler_wrapped, GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 0);

	// Software syscall gate for .run programs (see x86_64/syscall.h).
	// DPL3 so it stays callable once real ring3 processes exist; every
	// process today still runs at CPL0, so the DPL check is currently
	// moot but costs nothing to set correctly now.
	idt_set_entry(0x80, (uint64_t)isr_syscall_wrapped, GDT_SELECTOR_CS_KERNEL,
	              IDT_PRESENT | IDT_DPL3 | IDT_TYPE_INTERRUPT_GATE, 0);

	idt_load(&idt_ptr);
	
	sti();
}

void idt_set_handler_keyboard(void (*handler)()) {
	idt_handler_keyboard_user = handler;
}

void idt_handler_pit() {
	if (idt_handler_pit_user) {
		idt_handler_pit_user();
	}
	pic_eoi_master();
}

void idt_handler_doublefault() {
    PANIC("Double fault!");
    while (1) __asm__("hlt");
}

void idt_set_handler_pit(void (*handler)()) {
	idt_handler_pit_user = handler;
}
