#include "x86_64/audio_manager.h"
#include "serial.h"
#include <string.h>

static const audio_hw_driver_t* g_audio_driver = NULL;
static audio_mix_cb_t g_mix_cb = NULL;
static audio_ready_cb_t g_ready_cb = NULL;

void audio_manager_register_driver(const audio_hw_driver_t* drv) {
    g_audio_driver = drv;
}

bool audio_manager_init(void) {
    if (!g_audio_driver) return false;
    serial_write_str("audio_manager: initializing driver\n");
    if (g_audio_driver->init) return g_audio_driver->init();
    return false;
}

void audio_manager_update(void) {
    if (g_audio_driver && g_audio_driver->update) g_audio_driver->update();
}

void audio_manager_set_mix_callback(audio_mix_cb_t cb) { g_mix_cb = cb; }
void audio_manager_set_ready_callback(audio_ready_cb_t cb) { g_ready_cb = cb; }

void audio_manager_pull_samples(int16_t* out, uint32_t frames) {
    if (!g_mix_cb) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    g_mix_cb(out, frames);
}

bool audio_manager_has_data(void) {
    if (!g_ready_cb) return false;
    return g_ready_cb();
}

void audio_manager_start(void) {
    if (g_audio_driver && g_audio_driver->start) g_audio_driver->start();
}

void audio_manager_set_sample_rate(uint32_t rate_hz) {
    if (g_audio_driver && g_audio_driver->set_sample_rate) g_audio_driver->set_sample_rate(rate_hz);
}
