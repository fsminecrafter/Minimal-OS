#pragma once
#include <stdint.h>
#include "x86_64/fgpu_abi.h"

// SYS_GPU entry (rdi = syscall_gpu_request_t*).
uint64_t fgpu_syscall(syscall_gpu_request_t* request);

// Frees a process's GPU surfaces/buffers and undoes its page flip.
void fgpu_process_exit(uint64_t pid);
