#pragma once

#include <stddef.h>
#include <stdbool.h>

typedef void (*interrupt_func_t)(void);

void triggerinterrupt(interrupt_func_t func);
void assigninterrupt(bool* condition, interrupt_func_t func);

void triggerinterrupt_o(interrupt_func_t func, size_t limit);
void assigninterrupt_o(bool* condition, interrupt_func_t func, size_t limit);
void reset_o();

void interruptdispatcher_tick(); // This will be called by the PIT handler
