#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_abi.h"
#include "x86_64/syscall.h"

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

// SYS_TASK_GPU: the graphics card, its VRAM and its engines' use.
void fgpu_gpu_stats(syscall_gpu_stats_t* out);
// 'gpu verify': compare the card's draws of the next `frames` frames with the CPU's, and the report.
void fgpu_verify_start(uint32_t frames);
void fgpu_verify_report(void (*print)(const char* text));
// 'gpu perf': where each frame's time went since the command last ran.
void fgpu_perf_report(void (*print)(const char* text));
