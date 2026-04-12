#pragma once

#include <stdbool.h>

typedef struct {
    int16_t* pcm;
    uint32_t size;
    uint32_t read_pos;
    uint32_t write_pos;
    bool ready;
} audio_ring_t;

static audio_ring_t g_audio_ring;

bool ac97_init(void);
void ac97_update(void);
void ac97_kick(void);
void ac97_set_sample_rate(uint32_t rate_hz);
