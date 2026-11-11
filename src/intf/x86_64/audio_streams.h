#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/syscall.h"

/*
 * Audio streams (SYS_AUDIO_STREAM): PCM that programs write, mixed on top
 * of the file player in audio_mix_streams(). audio_streams.c; no other
 * kernel dependencies (tests/host/audio_stream_tests.c builds it).
 *
 * Each stream is a ring of stereo frames with one writer (the program,
 * through the syscall) and one reader (the mixer, from the sound card's
 * update): the writer only moves `head`, the reader only `tail`, so they
 * need no lock.
 */

// The syscall; `pid` owns streams it opens.
uint64_t audio_stream_syscall(syscall_audio_stream_t* r, uint64_t pid);
// Adds every stream's next `frames` stereo frames, resampled to
// `mix_rate`, to `out` (saturating).
void audio_streams_mix(int16_t* out, uint32_t frames, uint32_t mix_rate);
// True when some stream has frames waiting.
bool audio_streams_have_data(void);
// Closes the streams `pid` left open.
void audio_streams_process_exit(uint64_t pid) __attribute__((weak));

static inline void audio_streams_on_process_exit(uint64_t pid) {
    if (audio_streams_process_exit) audio_streams_process_exit(pid);
}

// audio.c: the rate the mixer runs at now, and starting the sound card if
// nothing has yet. Called by audio_streams.c.
uint32_t audio_mix_rate(void);
void audio_ensure_running(void);
bool audio_have_device(void);           // a sound card was found
