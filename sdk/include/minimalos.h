#ifndef MINIMALOS_H
#define MINIMALOS_H

#include <stddef.h>
#include "minimalos/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t minimalos_write(int fd, const void* buffer, size_t size) {
    return minimalos_syscall3(MINIMALOS_SYS_WRITE, (uint64_t)fd,
                               (uint64_t)buffer, (uint64_t)size);
}

static inline void minimalos_exit(void) {
    (void)minimalos_syscall0(MINIMALOS_SYS_EXIT);
    for (;;) {
    }
}

static inline uint64_t minimalos_getpid(void) {
    return minimalos_syscall0(MINIMALOS_SYS_GETPID);
}

static inline uint64_t minimalos_uptime_ms(void) {
    return minimalos_syscall0(MINIMALOS_SYS_UPTIME);
}

static inline uint64_t minimalos_sleep(uint64_t milliseconds) {
    return minimalos_syscall1(MINIMALOS_SYS_SLEEP, milliseconds);
}

static inline minimalos_handle_t minimalos_open(const char* path) {
    return minimalos_syscall2(MINIMALOS_SYS_OPEN, (uint64_t)path,
                               MINIMALOS_O_RDONLY);
}

static inline uint64_t minimalos_read(minimalos_handle_t handle, void* buffer,
                                      size_t size) {
    return minimalos_syscall3(MINIMALOS_SYS_READ, handle, (uint64_t)buffer,
                               (uint64_t)size);
}

static inline uint64_t minimalos_close(minimalos_handle_t handle) {
    return minimalos_syscall1(MINIMALOS_SYS_CLOSE, handle);
}

static inline uint64_t minimalos_seek(minimalos_handle_t handle,
                                      uint32_t offset) {
    return minimalos_syscall2(MINIMALOS_SYS_SEEK, handle, offset);
}

static inline uint64_t minimalos_size(minimalos_handle_t handle) {
    return minimalos_syscall1(MINIMALOS_SYS_SIZE, handle);
}

static inline uint64_t minimalos_exists(const char* path) {
    return minimalos_syscall1(MINIMALOS_SYS_EXISTS, (uint64_t)path);
}

static inline uint64_t minimalos_is_dir(const char* path) {
    return minimalos_syscall1(MINIMALOS_SYS_IS_DIR, (uint64_t)path);
}

static inline uint64_t minimalos_mkdir(const char* path) {
    return minimalos_syscall1(MINIMALOS_SYS_MKDIR, (uint64_t)path);
}

static inline uint64_t minimalos_gettime(minimalos_datetime_t* output) {
    return minimalos_syscall1(MINIMALOS_SYS_GETTIME, (uint64_t)output);
}

#ifdef __cplusplus
}
#endif

#endif
