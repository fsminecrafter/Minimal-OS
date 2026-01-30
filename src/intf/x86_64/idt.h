#pragma once

#include <stdint.h>

extern void lidt(struct IDTPointer* idtp);

void idt_init();
void idt_set_handler_keyboard(void (*handler)());
void idt_handler_pit();
void idt_set_handler_pit(void (*handler)());