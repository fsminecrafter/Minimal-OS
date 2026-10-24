#pragma once
#include <stdint.h>
#include "x86_64/proc.h"

// See wm_watchdog.c: panics if mosWM stops calling into the kernel.
void wm_watchdog_syscall_enter(uint64_t num);
void wm_watchdog_syscall_exit(void);
void wm_watchdog_forget(const process_t* proc);   // before a process_t is freed
void wm_watchdog_tick(void);                       // cpu0 PIT tick
