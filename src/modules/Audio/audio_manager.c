#include "x86_64/audio_manager.h"
#include "serial.h"
#include <string.h>

/* Every driver the build supports registers here; audio_manager_init()
 * then uses the first one whose hardware is present. */
#define AUDIO_MANAGER_MAX_DRIVERS 4

static const audio_hw_driver_t* g_drivers[AUDIO_MANAGER_MAX_DRIVERS];
static int g_driver_count = 0;
static const audio_hw_driver_t* g_audio_driver = NULL;   /* the one in use */
static audio_mix_cb_t g_mix_cb = NULL;
static audio_ready_cb_t g_ready_cb = NULL;

void audio_manager_register_driver(const audio_hw_driver_t* drv) {
    if (!drv) return;
    for (int i = 0; i < g_driver_count; i++)
        if (g_drivers[i] == drv) return;
    if (g_driver_count < AUDIO_MANAGER_MAX_DRIVERS)
        g_drivers[g_driver_count++] = drv;
}

bool audio_manager_init(void) {
    if (g_audio_driver) return true;
    for (int i = 0; i < g_driver_count; i++) {
        const audio_hw_driver_t* drv = g_drivers[i];
        if (!drv->init) continue;
        serial_write_str("audio_manager: trying ");
        serial_write_str(drv->name ? drv->name : "(unnamed)");
        serial_write_str("\n");
        if (drv->init()) {
            g_audio_driver = drv;
            serial_write_str("audio_manager: using ");
            serial_write_str(drv->name ? drv->name : "(unnamed)");
            serial_write_str("\n");
            return true;
        }
    }
    return false;
}

const char* audio_manager_driver_name(void) {
    return (g_audio_driver && g_audio_driver->name) ? g_audio_driver->name : NULL;
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
