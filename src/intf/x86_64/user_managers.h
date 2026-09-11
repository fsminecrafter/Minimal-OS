#ifndef USER_MANAGERS_H
#define USER_MANAGERS_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

static inline uint64_t mos_gpu_init(void) {
    return mos_manager(SYS_MANAGER_GPU, SYS_MANAGER_INIT, 0);
}

static inline uint64_t mos_audio_init(void) {
    return mos_manager(SYS_MANAGER_AUDIO, SYS_MANAGER_INIT, 0);
}

static inline uint64_t mos_audio_update(void) {
    return mos_manager(SYS_MANAGER_AUDIO, SYS_MANAGER_UPDATE, 0);
}

static inline uint64_t mos_audio_start(void) {
    return mos_manager(SYS_MANAGER_AUDIO, SYS_MANAGER_START, 0);
}

static inline uint64_t mos_audio_has_data(void) {
    return mos_manager(SYS_MANAGER_AUDIO, SYS_MANAGER_HAS_DATA, 0);
}

static inline uint64_t mos_audio_set_sample_rate(uint32_t rate_hz) {
    return mos_manager(SYS_MANAGER_AUDIO, SYS_MANAGER_SET_SAMPLE_RATE, rate_hz);
}

static inline uint64_t mos_storage_init(void) {
    return mos_manager(SYS_MANAGER_STORAGE, SYS_MANAGER_INIT, 0);
}

static inline uint64_t mos_usb_manager_init(void) {
    return mos_manager(SYS_MANAGER_USB, SYS_MANAGER_INIT, 0);
}

#endif