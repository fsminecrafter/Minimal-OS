#ifndef MINIMALOS_SYSCALL_H
#define MINIMALOS_SYSCALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} minimalos_datetime_t;

typedef uint64_t minimalos_handle_t;

enum {
    MINIMALOS_SYS_WRITE = 1,
    MINIMALOS_SYS_EXIT = 2,
    MINIMALOS_SYS_GETPID = 3,
    MINIMALOS_SYS_UPTIME = 4,
    MINIMALOS_SYS_SLEEP = 5,
    MINIMALOS_SYS_OPEN = 6,
    MINIMALOS_SYS_READ = 7,
    MINIMALOS_SYS_CLOSE = 8,
    MINIMALOS_SYS_EXISTS = 9,
    MINIMALOS_SYS_IS_DIR = 10,
    MINIMALOS_SYS_GETTIME = 11,
    MINIMALOS_SYS_SEEK = 12,
    MINIMALOS_SYS_SIZE = 13,
    MINIMALOS_SYS_MKDIR = 14
};

enum {
    MINIMALOS_STDIN = 0,
    MINIMALOS_STDOUT = 1,
    MINIMALOS_STDERR = 2,
    MINIMALOS_O_RDONLY = 0
};

#define MINIMALOS_ERR_GENERIC ((uint64_t)-1)
#define MINIMALOS_ERR_BADFD ((uint64_t)-2)
#define MINIMALOS_ERR_NOTFOUND ((uint64_t)-3)
#define MINIMALOS_ERR_INVAL ((uint64_t)-4)

static inline uint64_t minimalos_syscall0(uint64_t number) {
    uint64_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number) : "memory");
    return result;
}

static inline uint64_t minimalos_syscall1(uint64_t number, uint64_t arg1) {
    uint64_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number), "D"(arg1) : "memory");
    return result;
}

static inline uint64_t minimalos_syscall2(uint64_t number, uint64_t arg1,
                                           uint64_t arg2) {
    uint64_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number), "D"(arg1),
                      "S"(arg2) : "memory");
    return result;
}

static inline uint64_t minimalos_syscall3(uint64_t number, uint64_t arg1,
                                           uint64_t arg2, uint64_t arg3) {
    uint64_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number), "D"(arg1),
                      "S"(arg2), "d"(arg3) : "memory");
    return result;
}

#ifdef __cplusplus
}
#endif

#endif
