#pragma once

#include <stdint.h>

struct IdtPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_load(struct IdtPtr* idt_ptr);

void idt_init(void);
void idt_set_handler_keyboard(void (*handler)());
void idt_handler_pit();
void idt_set_handler_pit(void (*handler)());
