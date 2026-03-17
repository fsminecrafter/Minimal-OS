#pragma once

#include <stdbool.h>

bool ac97_init(void);
void ac97_update(void);
void ac97_kick(void);
void ac97_set_sample_rate(uint32_t rate_hz);
