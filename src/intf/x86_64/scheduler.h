#pragma once

#include "proc.h"

extern process_t* current_process;

void scheduler_init();
void scheduler_tick(); // Call from PIT
void schedule();       // Switch to next process
void context_switch(process_t* current, process_t* next);
void ready(); //Ready process state
void currentstate();