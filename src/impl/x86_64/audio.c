#include "audio.h"
#include "serial.h"
#include "string.h"
#include "x86_64/ac97_driver.h"

static audio_stream_t g_streams[AUDIO_MAX_STREAMS];
static uint8_t g_master_volume = 100;
static bool g_master_muted = false;
static bool g_audio_initialized = false;

void audio_init(void) {
    serial_write_str("Audio: Initializing...\n");
    memset(g_streams, 0, sizeof(g_streams));
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        g_streams[i].id = i;
        g_streams[i].active = false;
        g_streams[i].volume = 100;
    }
    g_master_volume = 100;
    g_master_muted = false;
    g_audio_initialized = true;
    serial_write_str("Audio: Ready\n");
}

void audio_shutdown(void) {
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) audio_stop(i);
    g_audio_initialized = false;
}

int audio_create_stream(void) {
    if (!g_audio_initialized) return -1;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        if (!g_streams[i].active) {
            g_streams[i].active = true;
            g_streams[i].state = STREAM_STATE_STOPPED;
            g_streams[i].sound = NULL;
            g_streams[i].position = 0;
            g_streams[i].volume = 100;
            g_streams[i].muted = false;
            return i;
        }
    }
    return -1;
}

void audio_destroy_stream(int stream) {
    if (stream < 0 || stream >= AUDIO_MAX_STREAMS) return;
    audio_stop(stream);
    g_streams[stream].active = false;
}

void audio_play(const audio_sound_t* sound, int stream, uint8_t volume, bool loop) {
    if (stream < 0 || stream >= AUDIO_MAX_STREAMS) return;
    if (!g_streams[stream].active || !sound) return;
    if (sound->sample_rate > 0) {
        ac97_set_sample_rate(sound->sample_rate);
    }
    g_streams[stream].sound = sound;
    g_streams[stream].position = 0;
    g_streams[stream].volume = volume > 100 ? 100 : volume;
    g_streams[stream].loop = loop;
    g_streams[stream].state = STREAM_STATE_PLAYING;
    ac97_kick();
}

void audio_pause(int stream) {
    if (stream >= 0 && stream < AUDIO_MAX_STREAMS && g_streams[stream].state == STREAM_STATE_PLAYING)
        g_streams[stream].state = STREAM_STATE_PAUSED;
}

void audio_unpause(int stream) {
    if (stream >= 0 && stream < AUDIO_MAX_STREAMS && g_streams[stream].state == STREAM_STATE_PAUSED)
        g_streams[stream].state = STREAM_STATE_PLAYING;
}

void audio_stop(int stream) {
    if (stream >= 0 && stream < AUDIO_MAX_STREAMS) {
        g_streams[stream].state = STREAM_STATE_STOPPED;
        g_streams[stream].position = 0;
    }
}

void audio_set_volume(int stream, uint8_t volume) {
    if (stream >= 0 && stream < AUDIO_MAX_STREAMS)
        g_streams[stream].volume = volume > 100 ? 100 : volume;
}

uint8_t audio_get_volume(int stream) {
    return (stream >= 0 && stream < AUDIO_MAX_STREAMS) ? g_streams[stream].volume : 0;
}

void audio_mute(int stream) {
    if (stream >= 0 && stream < AUDIO_MAX_STREAMS) g_streams[stream].muted = true;
}

void audio_unmute(int stream) {
    if (stream >= 0 && stream < AUDIO_MAX_STREAMS) g_streams[stream].muted = false;
}

bool audio_is_muted(int stream) {
    return (stream >= 0 && stream < AUDIO_MAX_STREAMS) ? g_streams[stream].muted : true;
}

bool audio_is_playing(int stream) {
    return (stream >= 0 && stream < AUDIO_MAX_STREAMS) ? g_streams[stream].state == STREAM_STATE_PLAYING : false;
}

void audio_set_master_volume(uint8_t volume) {
    g_master_volume = volume > 100 ? 100 : volume;
}

uint8_t audio_get_master_volume(void) {
    return g_master_volume;
}

void audio_mute_all(void) {
    g_master_muted = true;
}

void audio_unmute_all(void) {
    g_master_muted = false;
}

static int16_t audio_get_sample(audio_stream_t* stream, bool right_channel) {
    if (!stream->sound) return 0;
    const uint8_t* data = stream->sound->data;
    uint32_t pos = stream->position;
    uint16_t bits = stream->sound->bits_per_sample;
    uint16_t channels = stream->sound->channels;
    uint32_t sample_index = pos;
    if (channels == 2) sample_index = pos * 2 + (right_channel ? 1 : 0);
    int32_t sample = 0;
    if (bits == 8) {
        sample = ((int32_t)data[sample_index] - 128) << 8;
    } else if (bits == 16) {
        uint32_t byte_offset = sample_index * 2;
        sample = *((int16_t*)(data + byte_offset));
    }
    sample = (sample * stream->volume) / 100;
    return (int16_t)AUDIO_CLAMP(sample);
}

void audio_mix_streams(int16_t* output, uint32_t num_samples) {
    if (!output || !g_audio_initialized) return;
    for (uint32_t i = 0; i < num_samples * 2; i++) output[i] = 0;
    if (g_master_muted) return;
    for (int s = 0; s < AUDIO_MAX_STREAMS; s++) {
        audio_stream_t* stream = &g_streams[s];
        if (!stream->active || stream->state != STREAM_STATE_PLAYING || !stream->sound || stream->muted) continue;
        for (uint32_t i = 0; i < num_samples; i++) {
            if (stream->position >= stream->sound->num_samples) {
                if (stream->loop) stream->position = 0;
                else { stream->state = STREAM_STATE_STOPPED; break; }
            }
            int16_t left = audio_get_sample(stream, false);
            int16_t right = audio_get_sample(stream, true);
            int32_t mixed_left = output[i * 2] + left;
            int32_t mixed_right = output[i * 2 + 1] + right;
            output[i * 2] = AUDIO_CLAMP(mixed_left);
            output[i * 2 + 1] = AUDIO_CLAMP(mixed_right);
            stream->position++;
        }
    }
    if (g_master_volume != 100) {
        for (uint32_t i = 0; i < num_samples * 2; i++) {
            int32_t sample = (output[i] * g_master_volume) / 100;
            output[i] = AUDIO_CLAMP(sample);
        }
    }
}

void audio_update(void) {
    ac97_update();

    static uint32_t debug_tick = 0;
    debug_tick++;
    if ((debug_tick % 100) == 0) {
        uint8_t active = audio_get_active_streams();
        serial_write_str("Audio: update tick, active streams=");
        serial_write_dec(active);
        if (active > 0) {
            for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
                if (g_streams[i].active && g_streams[i].state == STREAM_STATE_PLAYING && g_streams[i].sound) {
                    serial_write_str(" stream=");
                    serial_write_dec(i);
                    serial_write_str(" pos=");
                    serial_write_dec(g_streams[i].position);
                    break;
                }
            }
        }
        serial_write_str("\n");
    }
}

void audio_load_wav(audio_sound_t* sound, const wav_header_t* header, const uint8_t* data, uint32_t data_size) {
    if (!sound || !header || !data) return;
    sound->header = header;
    sound->data = data;
    sound->data_size = data_size;
    sound->channels = header->num_channels;
    sound->bits_per_sample = header->bits_per_sample;
    sound->sample_rate = header->sample_rate;
    sound->audio_format = header->audio_format;
    sound->bitrate = header->byte_rate * 8;
    uint32_t bytes_per_sample = (header->bits_per_sample / 8) * header->num_channels;
    sound->num_samples = data_size / bytes_per_sample;
    if (header->byte_rate > 0) {
        sound->duration_ms = (data_size * 1000U) / header->byte_rate;
    } else {
        sound->duration_ms = 0;
    }
}

uint8_t audio_get_active_streams(void) {
    uint8_t count = 0;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++)
        if (g_streams[i].active && g_streams[i].state != STREAM_STATE_STOPPED) count++;
    return count;
}

const char* audio_get_status(void) {
    uint8_t active = audio_get_active_streams();
    if (active == 0) return "Audio: Idle";
    else if (active == 1) return "Audio: 1 stream";
    else return "Audio: Multiple streams";
}
