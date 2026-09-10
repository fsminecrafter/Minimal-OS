#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_GETPID  3
#define SYS_UPTIME  4
#define SYS_SLEEP   5
#define SYS_OPEN    6
#define SYS_READ    7
#define SYS_CLOSE   8
#define SYS_EXISTS  9
#define SYS_IS_DIR  10
#define SYS_GETTIME 11
#define SYS_SEEK    12
#define SYS_SIZE    13
#define SYS_MKDIR   14

#define SYS_O_RDONLY 0

#define SYS_SUCCESS       0
#define SYS_ERR_GENERIC  ((uint64_t)-1)
#define SYS_ERR_BADFD    ((uint64_t)-2)
#define SYS_ERR_NOTFOUND ((uint64_t)-3)
#define SYS_ERR_INVAL    ((uint64_t)-4)

// Layout MUST match the push order in syscall_isr.asm exactly.
typedef struct __attribute__((packed)) {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} syscall_regs_t;

// Called by isr_syscall_wrapped (syscall_isr.asm) for every `int 0x80`.
// regs->rax = syscall number in, return value out. Args in
// rdi/rsi/rdx (SysV-ish - matches a future `syscall`-instruction ABI
// too, if ring3 ever lands). Return values are in rax; failures are
// negative values represented as uint64_t.
void syscall_dispatch(syscall_regs_t* regs);

#endif // SYSCALL_H