#ifndef MINIMALOS_SDK_H
#define MINIMALOS_SDK_H

#include <stdint.h>
#include <stddef.h>

#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_GETPID  3

static inline long mos_syscall(long num, long a1, long a2, long a3) {
    long ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline long mos_write(int fd, const void* buf, size_t len) {
    return mos_syscall(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)len);
}

static inline long mos_getpid(void) {
    return mos_syscall(SYS_GETPID, 0, 0, 0);
}

static inline void mos_exit(int code) {
    mos_syscall(SYS_EXIT, code, 0, 0);
    for (;;) { } // never reached - SYS_EXIT never returns
}

// Convenience: write a NUL-terminated string to stdout.
static inline long mos_puts(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return mos_write(1, str, len);
}

#endif // MINIMALOS_SDK_H