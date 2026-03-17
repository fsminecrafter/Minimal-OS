#ifndef MUSIC_WRAPPER_H
#define MUSIC_WRAPPER_H

#include "audio.h"
#include <stdint.h>

/*
 * This wrapper creates an audio_sound_t from binary-embedded WAV data
 * 
 * Your Makefile does: objcopy -I binary -O elf64-x86-64 music.wav music.o
 * Which creates these symbols:
 *   _binary_music_wav_start
 *   _binary_music_wav_end
 */

extern const uint8_t _binary_music_wav_start[];
extern const uint8_t _binary_music_wav_end[];

// Parse WAV header from embedded binary (chunk-walking)
static inline const wav_header_t* get_music_header(void) {
    return (const wav_header_t*)_binary_music_wav_start;
}

static inline const uint8_t* find_wav_chunk(const uint8_t* data, const uint8_t* end, const char id[4]) {
    const uint8_t* p = data + 12; // skip RIFF header
    while (p + 8 <= end) {
        const char* cid = (const char*)p;
        uint32_t size = *(const uint32_t*)(p + 4);
        if (cid[0] == id[0] && cid[1] == id[1] && cid[2] == id[2] && cid[3] == id[3]) {
            return p;
        }
        p += 8 + size;
        if (p > end) break;
    }
    return NULL;
}

// Get audio data pointer and size by scanning chunks
static inline const uint8_t* get_music_data_ptr(uint32_t* out_size) {
    const uint8_t* start = _binary_music_wav_start;
    const uint8_t* end = _binary_music_wav_end;
    const uint8_t* data_chunk = find_wav_chunk(start, end, "data");
    if (!data_chunk) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    uint32_t size = *(const uint32_t*)(data_chunk + 4);
    if (out_size) *out_size = size;
    return data_chunk + 8;
}

static inline bool get_music_fmt(uint16_t* audio_format, uint16_t* num_channels,
                                 uint32_t* sample_rate, uint32_t* byte_rate,
                                 uint16_t* block_align, uint16_t* bits_per_sample) {
    const uint8_t* start = _binary_music_wav_start;
    const uint8_t* end = _binary_music_wav_end;
    const uint8_t* fmt_chunk = find_wav_chunk(start, end, "fmt ");
    if (!fmt_chunk) return false;
    const uint8_t* p = fmt_chunk + 8;
    *audio_format = *(const uint16_t*)(p + 0);
    *num_channels = *(const uint16_t*)(p + 2);
    *sample_rate = *(const uint32_t*)(p + 4);
    *byte_rate = *(const uint32_t*)(p + 8);
    *block_align = *(const uint16_t*)(p + 12);
    *bits_per_sample = *(const uint16_t*)(p + 14);
    return true;
}

// Create audio_sound_t structure
static inline void init_music_sound(audio_sound_t* sound) {
    const wav_header_t* header = get_music_header();
    uint32_t data_size = 0;
    const uint8_t* data_ptr = get_music_data_ptr(&data_size);
    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint32_t byte_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    if (!get_music_fmt(&audio_format, &num_channels, &sample_rate, &byte_rate, &block_align, &bits_per_sample)) {
        audio_format = header->audio_format;
        num_channels = header->num_channels;
        sample_rate = header->sample_rate;
        byte_rate = header->byte_rate;
        block_align = header->block_align;
        bits_per_sample = header->bits_per_sample;
    }

    sound->name = "music";
    sound->header = header;
    sound->data = data_ptr;
    sound->data_size = data_size;
    sound->channels = num_channels;
    sound->bits_per_sample = bits_per_sample;
    sound->sample_rate = sample_rate;
    sound->audio_format = audio_format;
    sound->bitrate = byte_rate * 8;
    
    // Calculate number of samples
    uint32_t bytes_per_sample = (bits_per_sample / 8) * num_channels;
    sound->num_samples = (bytes_per_sample > 0) ? (data_size / bytes_per_sample) : 0;
    if (byte_rate > 0) {
        sound->duration_ms = (data_size * 1000U) / byte_rate;
    } else {
        sound->duration_ms = 0;
    }
}

#endif // MUSIC_WRAPPER_H
