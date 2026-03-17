#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "serial.h"
#include "audio.h"
#include "music_wrapper.h"

static int g_music_stream = -1;
static audio_sound_t g_music_sound;

void init_music(void) {
    audio_init();
    init_music_sound(&g_music_sound);
    g_music_stream = audio_create_stream();
    
    serial_write_str("Music loaded: ");
    serial_write_dec(g_music_sound.num_samples);
    serial_write_str(" samples\n");
    serial_write_str("Music meta: ");
    serial_write_dec(g_music_sound.sample_rate);
    serial_write_str(" Hz, ");
    serial_write_dec(g_music_sound.bits_per_sample);
    serial_write_str(" bit, ");
    serial_write_dec(g_music_sound.channels);
    serial_write_str(" ch, ");
    serial_write_dec(g_music_sound.bitrate);
    serial_write_str(" bps, ");
    serial_write_dec(g_music_sound.duration_ms);
    serial_write_str(" ms\n");
}

void cmd_play(int argc, const char** argv) {
    if (g_music_stream < 0) init_music();
    audio_play(&g_music_sound, g_music_stream, 75, false);
    graphics_write_textr("Playing music (nonlooping)\n");
}

void cmd_pause(int argc, const char** argv) {
    if (g_music_stream >= 0) {
        if (audio_is_playing(g_music_stream)) {
            audio_pause(g_music_stream);
            graphics_write_textr("Paused\n");
        } else {
            audio_unpause(g_music_stream);
            graphics_write_textr("Resumed\n");
        }
    }
}

void cmd_stop(int argc, const char** argv) {
    if (g_music_stream >= 0) {
        audio_stop(g_music_stream);
        graphics_write_textr("Stopped\n");
    }
}

void register_play(void) {
    command_register("play", cmd_play);
    command_register("pause", cmd_pause);
    command_register("stop", cmd_stop);
}

REGISTER_COMMAND(register_play);
