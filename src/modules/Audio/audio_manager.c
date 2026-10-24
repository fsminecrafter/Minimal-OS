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

static volatile uint32_t g_master_volume = 100;
static volatile bool g_muted;

void audio_manager_pull_samples(int16_t* out, uint32_t frames) {
    if (!g_mix_cb) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    g_mix_cb(out, frames);
    /* Master volume on a squared curve, so the slider feels even. */
    uint32_t v = g_muted ? 0u : g_master_volume;
    if (v >= 100u) return;
    int32_t gain = (int32_t)(v * v);                /* 0..10000 */
    for (uint32_t i = 0; i < frames * 2u; i++)
        out[i] = (int16_t)((int32_t)out[i] * gain / 10000);
}

void audio_manager_set_master_volume(uint32_t volume) {
    g_master_volume = volume > 100u ? 100u : volume;
}
uint32_t audio_manager_master_volume(void) { return g_master_volume; }
void audio_manager_set_muted(bool muted) { g_muted = muted; }
bool audio_manager_muted(void) { return g_muted; }

int audio_manager_output_count(void) {
    if (!g_audio_driver) return 0;
    return g_audio_driver->output_count ? g_audio_driver->output_count() : 1;
}

void audio_manager_output_name(int index, char* out, uint32_t size) {
    if (!out || !size) return;
    out[0] = '\0';
    if (!g_audio_driver) return;
    if (g_audio_driver->output_name) {
        g_audio_driver->output_name(index, out, size);
        return;
    }
    const char* name = g_audio_driver->name ? g_audio_driver->name : "Sound card";
    uint32_t i = 0;
    for (; name[i] && i + 1 < size; i++) out[i] = name[i];
    out[i] = '\0';
}

int audio_manager_output_current(void) {
    if (!g_audio_driver) return -1;
    return g_audio_driver->output_current ? g_audio_driver->output_current() : 0;
}

bool audio_manager_output_select(int index) {
    if (!g_audio_driver || index < 0 || index >= audio_manager_output_count()) return false;
    if (!g_audio_driver->output_select) return index == 0;
    return g_audio_driver->output_select(index);
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
