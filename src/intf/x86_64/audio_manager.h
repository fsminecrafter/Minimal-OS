#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/audio_hw.h"

typedef void (*audio_mix_cb_t)(int16_t* out, uint32_t frames);
typedef bool (*audio_ready_cb_t)(void);

void audio_manager_register_driver(const audio_hw_driver_t* drv);
bool audio_manager_init(void);
void audio_manager_update(void);
void audio_manager_set_mix_callback(audio_mix_cb_t cb);
void audio_manager_set_ready_callback(audio_ready_cb_t cb);
void audio_manager_pull_samples(int16_t* out, uint32_t frames);
bool audio_manager_has_data(void);
void audio_manager_start(void);
void audio_manager_set_sample_rate(uint32_t rate_hz);
