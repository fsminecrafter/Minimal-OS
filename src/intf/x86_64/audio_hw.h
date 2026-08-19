#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Default hardware audio parameters.
 * Drivers may override these by defining the macros before
 * including audio.h / audio_hw.h, but provide sensible
 * fallbacks to keep the core code compilable.
 */
#ifndef AUDIO_HW_BUFFER_FRAMES
#define AUDIO_HW_BUFFER_FRAMES 1024
#endif

#ifndef AUDIO_HW_CHANNELS
#define AUDIO_HW_CHANNELS 2
#endif

#ifndef AUDIO_HW_RING_SIZE
#define AUDIO_HW_RING_SIZE 4
#endif

typedef struct {
    const char* name;
    bool (*init)(void);
    void (*start)(void);
    void (*update)(void);
    void (*set_sample_rate)(uint32_t rate_hz);
    void (*shutdown)(void);
} audio_hw_driver_t;
