#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/minimafs.h"

// ===========================================
// AUDIO CONFIG DEFAULTS
// ===========================================
// These can be overridden by defining them before including audio.h
#ifndef AUDIO_BUFFER_SIZE
#define AUDIO_BUFFER_SIZE 8192  // frames per DMA buffer
#endif

#ifndef AUDIO_CHANNELS
#define AUDIO_CHANNELS 2         // stereo
#endif

// ===========================================
// AUDIO HARDWARE HOOKS
// ===========================================
// Implemented by the platform audio driver (e.g., AC97)
void audio_init(void);
void audio_update(void);
void audio_mix_streams(int16_t* out, uint32_t frames);

/*
 * Audio Streaming System for MinimalOS
 *
 * Supports:
 * - IMA ADPCM compression (4:1 ratio)
 * - Microsoft ADPCM compression (4:1 ratio)
 * - FLAC lossless compression (2:1 typical)
 * - Streaming playback for files > 1MB
 * - .adi unified audio format
 */

// ===========================================
// AUDIO FORMATS
// ===========================================

typedef enum {
    AUDIO_FORMAT_NONE = 0,
    AUDIO_FORMAT_IMA_ADPCM,      // IMA ADPCM (4-bit)
    AUDIO_FORMAT_MS_ADPCM,       // Microsoft ADPCM (4-bit)
    AUDIO_FORMAT_FLAC,           // FLAC lossless
    AUDIO_FORMAT_PCM16           // Uncompressed 16-bit PCM
} audio_format_t;

// ===========================================
// ADI FILE FORMAT
// ===========================================

typedef struct {
    audio_format_t format;
    uint32_t sample_rate;        // Hz (e.g., 44100)
    uint8_t channels;            // 1=mono, 2=stereo
    uint32_t length_seconds;     // Duration in seconds
    uint32_t data_length;        // Compressed data size in bytes
    uint8_t global_volume;       // 0-100
    uint32_t data_offset;        // Offset to audio data in file
} adi_header_t;

// ===========================================
// STREAMING
// ===========================================

typedef struct {
    char path[MINIMAFS_MAX_PATH];
    minimafs_file_handle_t* file_handle;

    bool write_mode;
    uint32_t chunk_size;         // Bytes per chunk
    bool include_metadata;       // Include ADI header?

    // Current state
    uint32_t current_offset;     // Current position in file
    uint32_t data_offset;   // absolute file offset of the first ADI audio byte
    uint32_t total_size;         // Total file size
    uint8_t* buffer;             // Chunk buffer
    uint8_t* prefetch_buf;     // leftover audio bytes from header scan
    uint32_t prefetch_size;    // how many bytes are in prefetch_buf
    uint32_t prefetch_pos;     // how many have been consumed
    bool end_of_stream;

    // ADI metadata (if audio file)
    adi_header_t adi_header;
    bool is_adi;
} audio_datastream_t;

/**
 * Create a data stream for reading/writing files in chunks
 *
 * @param path File path (e.g., "0:/music.adi")
 * @param write_mode true=write, false=read
 * @param chunk_size Bytes per chunk (e.g., 512, 1024, 4096)
 * @param include_metadata Read/write ADI header automatically
 * @return Stream handle or NULL on error
 */
audio_datastream_t* streamfile(const char* path, bool write_mode,
                               uint32_t chunk_size, bool include_metadata);

/**
 * Read next chunk from stream
 *
 * @param stream Stream handle
 * @param bytes_read Output: number of bytes actually read
 * @return Pointer to chunk data, or NULL on error/EOF
 */
uint8_t* readstream(audio_datastream_t* stream, uint32_t* bytes_read);

/**
 * Write chunk to stream
 *
 * @param stream Stream handle
 * @param data Data to write
 * @param size Size of data
 * @return true on success
 */
bool writestream(audio_datastream_t* stream, const uint8_t* data, uint32_t size);

/**
 * Update stream position (advance to next chunk)
 *
 * @param stream Stream handle
 * @return true if more data available
 */
bool updatestream(audio_datastream_t* stream);

/**
 * Seek to specific offset in stream
 */
bool seekstream(audio_datastream_t* stream, uint32_t offset);

/**
 * Get stream progress (0-100)
 */
uint8_t stream_progress(audio_datastream_t* stream);

/**
 * Close stream and free resources
 */
void closestream(audio_datastream_t* stream);

// ===========================================
// ADI FILE OPERATIONS
// ===========================================

/**
 * Parse ADI header from file
 *
 * @param path File path
 * @param header Output header structure
 * @return true on success
 */
bool adi_parse_header(const char* path, adi_header_t* header);

/**
 * Write ADI header to file
 */
bool adi_write_header(const char* path, const adi_header_t* header);

/**
 * Get ADI format name as string
 */
const char* adi_format_name(audio_format_t format);

// ===========================================
// AUDIO DECODERS
// ===========================================

/**
 * Decode IMA ADPCM to PCM16
 *
 * @param input Compressed IMA ADPCM data
 * @param input_size Size of compressed data
 * @param output Output PCM16 buffer (must be 4x input_size)
 * @param output_size Output: number of PCM16 samples produced
 * @param max_output Maximum samples to decode (prevents buffer overflow)
 * @return true on success
 */
bool decode_ima_adpcm(
    const uint8_t* input, uint32_t input_size,
    int16_t* output, uint32_t* output_size,
    uint32_t max_output,
    int* pred_l, int* step_l,
    int* pred_r, int* step_r
);

/**
 * Decode Microsoft ADPCM to PCM16
 */
bool decode_ms_adpcm(const uint8_t* input, uint32_t input_size,
                     int16_t* output, uint32_t* output_size);

/**
 * Decode FLAC to PCM16
 */
bool decode_flac(const uint8_t* input, uint32_t input_size,
                 int16_t* output, uint32_t* output_size);

// ===========================================
// AUDIO PLAYBACK
// ===========================================

/*
 * Ring buffer depth: how many decoded audio buffers the player keeps
 * ready ahead of what the mixer is currently draining.
 *
 * This must be deep enough to absorb normal scheduling jitter of the
 * background decode task (audio_player_update(), a cooperatively
 * scheduled process) PLUS the AHCI block read it occasionally triggers
 * when a decode chunk crosses into a fresh disk block. The old design
 * only ever kept 2 buffers ready (a strict front/back double-buffer),
 * which left the mixer with essentially zero slack: a single missed
 * scheduling slot, or one slightly slow disk read, was enough to starve
 * playback entirely. That's what caused the chronic "[AUDIO] underrun"
 * spam, the helicopter/droning artifact (AC97_BD_BUP repeating the last
 * real buffer it had), and the song audibly taking far longer to finish
 * than its real length. AC97's own fill-ahead target
 * (g_target_ahead in ac97_driver.c) was already 4 buffers deep - the
 * player just never had that much decoded audio available to give it.
 *
 * 4 is not an arbitrary choice: MinimaFS blocks are 4096 bytes, and the
 * IMA-ADPCM decode chunk size is computed as pcm_capacity/2 bytes,
 * which for a stereo buffer works out to exactly 1024 bytes - i.e.
 * four decode chunks per disk block. A ring of 4 means one AHCI block
 * read now refills the *entire* ring in one shot, instead of leaving
 * the player perpetually one chunk-read away from running dry.
 */
#define AUDIO_RING_SIZE 8

typedef struct {
    audio_datastream_t* stream;
    audio_format_t format;

    int16_t* pcm_buffer;          /* AUDIO_RING_SIZE slots, each pcm_capacity samples, contiguous */
    uint32_t pcm_capacity;        /* samples per slot (one hardware buffer's worth) */

    /*
     * Ring buffer state.
     *
     *   ring_ready[slot] - written by the background decode task,
     *                      read by the mixer. true = slot holds fresh
     *                      decoded PCM ready to play.
     *   ring_size[slot]  - valid sample count in that slot. Only
     *                      meaningful while ring_ready[slot] is true.
     *
     * Consumer (mixer, runs from PIT-interrupt context via
     * audio_mix_streams()): drains read_slot from read_pos up to
     * ring_size[read_slot], then clears ring_ready[read_slot] (freeing
     * it for the producer), advances read_slot, and starts draining
     * the next slot once it's ready.
     *
     * Producer (background task, audio_player_update()): walks
     * forward from write_slot filling every slot that isn't
     * ring_ready yet, so a single call can catch up on more than one
     * slot at once if it fell behind.
     */
    volatile bool ring_ready[AUDIO_RING_SIZE];
    uint32_t      ring_size[AUDIO_RING_SIZE];

    uint8_t  read_slot;   /* slot the mixer is currently draining */
    uint32_t read_pos;    /* position within read_slot */
    uint8_t  write_slot;  /* next slot the decoder should try to fill */

    int adpcm_pred_l;
    int adpcm_step_l;
    int adpcm_pred_r;
    int adpcm_step_r;

    uint8_t volume;              /* 0-100 */
    bool playing;
    bool loop;
} audio_player_t;

/**
 * Create audio player for file
 *
 * @param path Path to .adi file
 * @return Player handle or NULL on error
 */
audio_player_t* audio_player_create(const char* path);

/**
 * Start playback
 */
void audio_player_play(audio_player_t* player);

/**
 * Pause playback
 */
void audio_player_pause(audio_player_t* player);

/**
 * Stop playback
 */
void audio_player_stop(audio_player_t* player);

/**
 * Set volume (0-100)
 */
void audio_player_set_volume(audio_player_t* player, uint8_t volume);

/**
 * Update player (call periodically to stream more data)
 * Returns true if still playing
 */
bool audio_player_update(audio_player_t* player);

/**
 * Get playback position (0-100%)
 */
uint8_t audio_player_get_progress(audio_player_t* player);

/**
 * Destroy player and free resources
 */
void audio_player_destroy(audio_player_t* player);

// ===========================================
// MINIMAFS STRING UTILITIES
// ===========================================

/**
 * Find substring in file
 *
 * @param needle String to search for
 * @param path File path
 * @return Offset where found, or -1 if not found
 */
int32_t findinfile(const char* needle, const char* path);

/**
 * Split string and get value
 *
 * Example: getvalfromsplit("Globalvol=80", "=", 2) returns "80"
 *
 * @param str String to split
 * @param delimiter Delimiter character
 * @param index Index of value to return (1-based)
 * @param output Output buffer
 * @param output_size Size of output buffer
 * @return true on success
 */
bool getvalfromsplit(const char* str, const char* delimiter,
                     int index, char* output, size_t output_size);

/**
 * Parse integer from string
 */
int32_t parse_int(const char* str);

#endif // AUDIO_H
