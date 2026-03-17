#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * MinimalOS Audio System
 * 
 * Features:
 * - Multiple audio streams with independent volume control
 * - Automatic mixing of all active streams
 * - WAV file support (embedded as headers)
 * - Play, pause, mute controls per stream
 * - Global volume control
 */

// ===========================================
// AUDIO CONFIGURATION
// ===========================================

#define AUDIO_MAX_STREAMS       8       // Maximum simultaneous audio streams
#define AUDIO_SAMPLE_RATE       48000   // 48.0 kHz sample rate
#define AUDIO_BUFFER_SIZE       8192    // Output buffer size in samples
#define AUDIO_CHANNELS          2       // Stereo output

// ===========================================
// WAV FILE FORMAT
// ===========================================

typedef struct {
    // RIFF Header
    char riff[4];           // "RIFF"
    uint32_t file_size;     // File size - 8
    char wave[4];           // "WAVE"
    
    // Format Chunk
    char fmt[4];            // "fmt "
    uint32_t fmt_size;      // Format chunk size (16 for PCM)
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;  // 1 = Mono, 2 = Stereo
    uint32_t sample_rate;   // Samples per second
    uint32_t byte_rate;     // Bytes per second
    uint16_t block_align;   // Bytes per sample * channels
    uint16_t bits_per_sample; // 8, 16, 24, 32
    
    // Data Chunk
    char data[4];           // "data"
    uint32_t data_size;     // Size of audio data
} __attribute__((packed)) wav_header_t;

typedef struct {
    const char* name;
    const wav_header_t* header;
    const uint8_t* data;
    uint32_t data_size;
    uint32_t num_samples;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t sample_rate;
    uint16_t audio_format; // 1 = PCM (uncompressed)
    uint32_t bitrate;      // bits per second
    uint32_t duration_ms;
} audio_sound_t;

typedef enum {
    STREAM_STATE_STOPPED = 0,
    STREAM_STATE_PLAYING,
    STREAM_STATE_PAUSED
} stream_state_t;

typedef struct {
    uint8_t id;
    bool active;
    stream_state_t state;
    const audio_sound_t* sound;
    uint32_t position;
    bool loop;
    uint8_t volume;
    bool muted;
    int32_t last_left;
    int32_t last_right;
} audio_stream_t;

// API Functions
void audio_init(void);
void audio_shutdown(void);

int audio_create_stream(void);
void audio_destroy_stream(int stream);
void audio_play(const audio_sound_t* sound, int stream, uint8_t volume, bool loop);
void audio_pause(int stream);
void audio_unpause(int stream);
void audio_stop(int stream);
void audio_set_volume(int stream, uint8_t volume);
uint8_t audio_get_volume(int stream);
void audio_mute(int stream);
void audio_unmute(int stream);
bool audio_is_muted(int stream);
bool audio_is_playing(int stream);

void audio_set_master_volume(uint8_t volume);
uint8_t audio_get_master_volume(void);
void audio_mute_all(void);
void audio_unmute_all(void);

void audio_mix_streams(int16_t* output, uint32_t num_samples);
void audio_update(void);
void audio_load_wav(audio_sound_t* sound, const wav_header_t* header, 
                    const uint8_t* data, uint32_t data_size);
uint8_t audio_get_active_streams(void);
const char* audio_get_status(void);

#define AUDIO_CLAMP(val) \
    ((val) > 32767 ? 32767 : ((val) < -32768 ? -32768 : (val)))

#endif // AUDIO_H
