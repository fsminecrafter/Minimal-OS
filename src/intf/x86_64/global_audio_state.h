#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    audio_player_t* player;
    bool playing;
} audio_state_t;

extern audio_state_t g_audio_state;