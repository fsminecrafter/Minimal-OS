#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_abi.h"

// SYS_GPU entry (rdi = syscall_gpu_request_t*).
uint64_t fgpu_syscall(syscall_gpu_request_t* request);

// Frees a process's GPU surfaces/buffers and undoes its page flip.
void fgpu_process_exit(uint64_t pid);

// Picks the hardware backend for the primary adapter (or none: CPU).
// Called by gpu_core_init() once the adapters are known.
void fgpu_backend_select(void);

// The 'fgpuold' boot flag: draw with the previous immediate CPU rasterizer
// (one parallel phase per draw) instead of the binned one. Read by
// fgpu_backend_select().
extern bool kernel_boot_fgpuold;

// Renders any draws the CPU backend has recorded but not drawn yet. Called
// before anything reads or writes surfaces, buffers or programs.
void fgpu_flush_pending(void);
