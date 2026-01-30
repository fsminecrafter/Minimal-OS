#include <stddef.h>
#include <stdint.h>
#include "x86_64/gdt.h"
#include "x86_64/idt.h"
#include "x86_64/pic.h"
#include "x86_64/scheduler.h"
#include "panic.h"
#include "print.h"

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

struct IdtPtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

struct IdtEntry idt[256] __attribute__((aligned(16)));
struct IdtPtr idt_ptr;

void (*idt_handler_keyboard_user)();

extern void idt_load(struct IdtPtr* idt_ptr);

extern void idt_handler_keyboard_wrapped();


void idt_reload() {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    lidt(&idt_ptr);
}

void idt_handler_keyboard() {
	if (idt_handler_keyboard_user != NULL) {
		idt_handler_keyboard_user();
	}
	
	pic_eoi_master();
}


extern struct IdtEntry idt[256];

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
	idt_set_entry(8, (uint64_t)idt_handler_doublefault_wrapped, GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 1);
	idt_set_entry(IDT_IRQ1_KEYBOARD, (uint64_t) idt_handler_keyboard_wrapped, GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 0);
	idt_set_entry(IDT_IRQ0_TIMER,    (uint64_t) idt_handler_pit_wrapped,      GDT_SELECTOR_CS_KERNEL, IDT_ENTRY_TYPE_INTERRUPT, 0);

	idt_load(&idt_ptr);
	
	asm volatile("sti");
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

__attribute__((noreturn))
void idt_handler_doublefault() {
    PANIC("Double fault!");
    while (1) __asm__("hlt");
}

void idt_set_handler_pit(void (*handler)()) {
	idt_handler_pit_user = handler;
}