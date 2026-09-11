#ifndef USER_SYSCALLS_H
#define USER_SYSCALLS_H

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/syscall.h"

static inline uint64_t mos_syscall3(uint64_t number, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3) {
    uint64_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

static inline uint64_t mos_syscall1(uint64_t number, uint64_t arg1) {
    return mos_syscall3(number, arg1, 0, 0);
}

static inline uint64_t mos_write(const char* text, uint64_t length) {
    return mos_syscall3(SYS_WRITE, 1, (uint64_t)text, length);
}

static inline void mos_exit(void) {
    mos_syscall1(SYS_EXIT, 0);
    for (;;) { }
}

static inline uint64_t mos_getpid(void) { return mos_syscall1(SYS_GETPID, 0); }
static inline uint64_t mos_uptime(void) { return mos_syscall1(SYS_UPTIME, 0); }
static inline void mos_sleep(uint64_t milliseconds) {
    mos_syscall1(SYS_SLEEP, milliseconds);
}

static inline uint64_t mos_graphics(syscall_graphics_request_t* request) {
    return mos_syscall1(SYS_GRAPHICS, (uint64_t)request);
}

static inline uint64_t mos_manager(uint64_t manager, uint64_t operation,
                                   uint64_t value) {
    return mos_syscall3(SYS_MANAGER, manager, operation, value);
}

static inline uint64_t mos_usb(uint64_t operation, uint64_t arg1,
                               uint64_t arg2) {
    return mos_syscall3(SYS_USB, operation, arg1, arg2);
}

#endif