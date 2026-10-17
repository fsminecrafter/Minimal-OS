#ifndef X86_64_ROOT_H
#define X86_64_ROOT_H

#include <stdint.h>
#include "x86_64/syscall.h"

// SYS_ROOT: password-checked kernel commands for user programs (root.c).
uint64_t root_syscall(syscall_root_t* request);

#endif
