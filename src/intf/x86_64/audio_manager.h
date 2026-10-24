#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/audio_hw.h"

typedef void (*audio_mix_cb_t)(int16_t* out, uint32_t frames);
typedef bool (*audio_ready_cb_t)(void);

// Adds a driver to the list audio_manager_init() tries, in order.
void audio_manager_register_driver(const audio_hw_driver_t* drv);
// Uses the first registered driver whose hardware initializes.
bool audio_manager_init(void);
// Name of the driver in use ("AC97", "Intel HD Audio"), or NULL.
const char* audio_manager_driver_name(void);
void audio_manager_update(void);
void audio_manager_set_mix_callback(audio_mix_cb_t cb);
void audio_manager_set_ready_callback(audio_ready_cb_t cb);
void audio_manager_pull_samples(int16_t* out, uint32_t frames);
bool audio_manager_has_data(void);
void audio_manager_start(void);
void audio_manager_set_sample_rate(uint32_t rate_hz);

/* Master volume (0-100) and mute, applied to everything played. */
void audio_manager_set_master_volume(uint32_t volume);
uint32_t audio_manager_master_volume(void);
void audio_manager_set_muted(bool muted);
bool audio_manager_muted(void);

/* Outputs of the sound card in use (at least 1 when there is one). */
int audio_manager_output_count(void);
void audio_manager_output_name(int index, char* out, uint32_t size);
int audio_manager_output_current(void);
bool audio_manager_output_select(int index);
