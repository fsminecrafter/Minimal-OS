#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/syscall.h"

// One system-wide player shared by the terminal's play/stop/pause/volume
// commands and SYS_AUDIO. Return values are SYS_SUCCESS / SYS_ERR_*.
uint64_t audio_ctl_open(const char* path);
uint64_t audio_ctl_pause(void);
uint64_t audio_ctl_resume(void);
uint64_t audio_ctl_stop(void);
uint64_t audio_ctl_seek_ms(uint32_t position_ms);
uint64_t audio_ctl_set_volume(uint32_t volume);
void     audio_ctl_status(syscall_audio_status_t* out);

// SYS_AUDIO entry point.
uint64_t audio_syscall(syscall_audio_request_t* req);
