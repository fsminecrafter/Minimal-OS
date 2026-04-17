#include "audio.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/ac97_driver.h"
#include "graphics.h"
#include "x86_64/exec_trace.h"
#include "x86_64/global_audio_state.h"

audio_state_t g_audio_state = {0};

/* ============================================================
 * Internal helpers
 * ============================================================ */

static int16_t audio_apply_volume(int16_t sample, uint8_t volume) {
    if (volume >= 100) return sample;
    return (int16_t)(((int32_t)sample * volume) / 100);
}

static uint32_t audio_min_u32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static const uint8_t* audio_find_bytes(const uint8_t* haystack, uint32_t haystack_len,
                                       const char* needle) {
    if (!haystack || !needle) return NULL;
    uint32_t needle_len = (uint32_t)strlen(needle);
    if (needle_len == 0 || haystack_len < needle_len) return NULL;
    for (uint32_t i = 0; i <= haystack_len - needle_len; i++) {
        uint32_t j = 0;
        for (; j < needle_len; j++) {
            if (haystack[i + j] != (uint8_t)needle[j]) break;
        }
        if (j == needle_len) return haystack + i;
    }
    return NULL;
}

static void audio_trim_ascii(char* s) {
    if (!s) return;
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        s[--len] = '\0';
    }
}

/* ============================================================
 * fill_buffer
 *
 * Reads one chunk from the stream, decodes it into the requested
 * half-buffer (buf_idx = 0 or 1), and returns the number of PCM
 * samples written.
 *
 * Chunk-size contract (CRITICAL):
 *   pcm_capacity = AUDIO_BUFFER_SIZE * channels  (samples per half)
 *
 *   Stereo IMA-ADPCM layout: 2 compressed bytes → 4 PCM samples
 *   (L0, R0, L1, R1).  So to fill pcm_capacity samples we need
 *   exactly pcm_capacity / 2 compressed bytes.
 *
 *   streamfile() is opened with chunk_size = pcm_capacity / 2, so
 *   each readstream() call delivers exactly the right amount.
 * ============================================================ */
static uint32_t audio_player_fill_buffer(audio_player_t* player, uint8_t buf_idx) {
    if (!player || !player->stream || !player->pcm_buffer) {
        serial_write_str("[AUDIO] fill_buffer: NULL player/stream/buffer\n");
        return 0;
    }
    if (player->pcm_capacity == 0) {
        serial_write_str("[AUDIO] fill_buffer: capacity is zero\n");
        return 0;
    }

    int16_t* target_buffer = player->pcm_buffer +
                             (uint32_t)buf_idx * player->pcm_capacity;

    uint32_t bytes_read = 0;
    uint8_t* compressed = readstream(player->stream, &bytes_read);

    if (!compressed || bytes_read == 0) {
        if (player->loop) {
            seekstream(player->stream, 0);
            compressed = readstream(player->stream, &bytes_read);
        }
        if (!compressed || bytes_read == 0) {
            serial_write_str("[AUDIO] fill_buffer: no data (EOF)\n");
            g_audio_state.playing = false;
            return 0;
        }
    }

    uint32_t decoded_samples = 0;
    bool     success         = false;

    switch (player->format) {
        case AUDIO_FORMAT_IMA_ADPCM:
            success = decode_ima_adpcm(
                compressed, bytes_read,
                target_buffer,
                &decoded_samples,
                player->pcm_capacity,
                &player->adpcm_pred_l,
                &player->adpcm_step_l,
                &player->adpcm_pred_r,
                &player->adpcm_step_r
            );
            break;

        case AUDIO_FORMAT_PCM16: {
            uint32_t max_bytes = player->pcm_capacity * sizeof(int16_t);
            uint32_t copy      = audio_min_u32(bytes_read, max_bytes);
            memcpy(target_buffer, compressed, copy);
            decoded_samples = copy / sizeof(int16_t);
            success = (decoded_samples > 0);
            break;
        }

        case AUDIO_FORMAT_MS_ADPCM:
            success = decode_ms_adpcm(compressed, bytes_read,
                                      target_buffer, &decoded_samples);
            break;

        case AUDIO_FORMAT_FLAC:
            success = decode_flac(compressed, bytes_read,
                                  target_buffer, &decoded_samples);
            break;

        default:
            success = false;
            break;
    }

    if (!success || decoded_samples == 0) {
        serial_write_str("[AUDIO] fill_buffer: decode failed/empty\n");
        g_audio_state.playing = false;
        return 0;
    }

    player->pcm_position = 0;
    player->pcm_size     = decoded_samples;
    return decoded_samples;
}

/* ============================================================
 * Hardware bridge
 * ============================================================ */

void audio_init(void) {
    ac97_init();
}

void audio_update(void) {
    ac97_update();
    if (g_audio_state.playing && g_audio_state.player) {
        audio_player_update(g_audio_state.player);
    }
}

/* ============================================================
 * audio_mix_streams
 *
 * Called by the AC97 driver to fill one hardware DMA buffer.
 * Pulls decoded PCM from the player's double-buffer.
 * ============================================================ */
void audio_mix_streams(int16_t* out, uint32_t frames) {
    if (!out || frames == 0) return;

    uint32_t out_samples = frames * AUDIO_CHANNELS;
    audio_player_t* player = g_audio_state.player;

    if (!player || !g_audio_state.playing) {
        memset(out, 0, out_samples * sizeof(int16_t));
        return;
    }

    if (!player->pcm_buffer) {
        memset(out, 0, out_samples * sizeof(int16_t));
        serial_write_str("[AUDIO] ERROR: pcm_buffer NULL in mix\n");
        return;
    }

    uint8_t src_channels = player->stream ?
                           player->stream->adi_header.channels : AUDIO_CHANNELS;
    if (src_channels == 0) src_channels = AUDIO_CHANNELS;

    const uint8_t out_channels = AUDIO_CHANNELS;
    const uint8_t volume       = (player->volume > 100) ? 100 : player->volume;

    int16_t* current_buffer = player->pcm_buffer +
                              (uint32_t)player->current_buffer * player->pcm_capacity;

    uint32_t out_idx = 0;

    while (out_idx < out_samples) {
        /* Buffer exhausted — try to refill */
        if (player->pcm_position >= player->pcm_size) {
            bool ok = audio_player_update(player);
            if (!ok || player->pcm_size == 0) {
                memset(out + out_idx, 0,
                       (out_samples - out_idx) * sizeof(int16_t));
                return;
            }
            /* Refresh pointer after buffer switch */
            current_buffer = player->pcm_buffer +
                             (uint32_t)player->current_buffer * player->pcm_capacity;
            continue;
        }

        if (src_channels <= 1) {
            /* Mono source → duplicate to all output channels */
            int16_t s = audio_apply_volume(
                current_buffer[player->pcm_position++], volume);
            if (out_channels == 1) {
                out[out_idx++] = s;
            } else {
                out[out_idx++] = s;
                if (out_idx < out_samples) out[out_idx++] = s;
            }
        } else {
            /* Stereo source: need at least 2 samples (L + R) */
            if (player->pcm_position + 2 > player->pcm_size) {
                /* Not enough samples left — let the top of the loop refill */
                player->pcm_position = player->pcm_size;
                continue;
            }

            int16_t l = audio_apply_volume(
                current_buffer[player->pcm_position++], volume);
            int16_t r = audio_apply_volume(
                current_buffer[player->pcm_position++], volume);

            /* Skip surplus channels if source has > 2 */
            if (src_channels > 2) {
                uint32_t skip = src_channels - 2;
                if (player->pcm_position + skip <= player->pcm_size)
                    player->pcm_position += skip;
                else
                    player->pcm_position = player->pcm_size;
            }

            if (out_channels == 1) {
                out[out_idx++] = (int16_t)(((int32_t)l + r) / 2);
            } else {
                out[out_idx++] = l;
                if (out_idx < out_samples) out[out_idx++] = r;
            }
        }
    }
}

/* ============================================================
 * String utilities
 * ============================================================ */

int32_t parse_int(const char* str) {
    if (!str) return 0;
    int32_t result  = 0;
    bool    negative = false;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-')      { negative = true; str++; }
    else if (*str == '+') { str++; }
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return negative ? -result : result;
}

/* ============================================================
 * Data streaming
 * ============================================================ */

bool adi_parse_header_from_stream(audio_datastream_t* stream, adi_header_t* header);

audio_datastream_t* streamfile(const char* path, bool write_mode,
                               uint32_t chunk_size, bool include_metadata) {
    if (!path || chunk_size == 0) return NULL;
    if (chunk_size > 1024 * 1024) {
        serial_write_str("[STREAMFILE] ERROR: chunk_size too large\n");
        return NULL;
    }

    audio_datastream_t* stream =
        (audio_datastream_t*)alloc(sizeof(audio_datastream_t));
    if (!stream) return NULL;

    memset(stream, 0, sizeof(audio_datastream_t));
    strncpy(stream->path, path, sizeof(stream->path) - 1);
    stream->write_mode      = write_mode;
    stream->chunk_size      = chunk_size;
    stream->include_metadata = include_metadata;
    stream->current_offset  = 0;
    stream->end_of_stream   = false;
    stream->data_offset     = 0;
    stream->total_size      = 0;

    stream->buffer = (uint8_t*)alloc(chunk_size);
    if (!stream->buffer) { free_mem(stream); return NULL; }

    stream->file_handle = minimafs_open(path, !write_mode);
    if (!stream->file_handle) {
        if (!write_mode) {
            serial_write_str("streamfile: Failed to open ");
            serial_write_str(path);
            serial_write_str("\n");
            free_mem(stream->buffer);
            free_mem(stream);
            return NULL;
        }
        return stream;
    }

    if (write_mode) return stream;

    uint32_t file_size  = stream->file_handle->data_size;
    stream->total_size  = file_size;

    if (include_metadata) {
        if (adi_parse_header_from_stream(stream, &stream->adi_header)) {
            stream->is_adi     = true;
            stream->data_offset = stream->adi_header.data_offset;

            uint32_t payload_size = 0;
            if (stream->adi_header.data_length > 0) {
                payload_size = stream->adi_header.data_length;
            } else if (file_size > stream->data_offset) {
                payload_size = file_size - stream->data_offset;
            }

            if (stream->data_offset >= file_size || payload_size == 0) {
                serial_write_str("streamfile: invalid ADI payload range\n");
                stream->is_adi = false;
            } else {
                uint32_t max_payload = file_size - stream->data_offset;
                if (payload_size > max_payload) payload_size = max_payload;
                stream->total_size     = payload_size;
                stream->current_offset = 0;
                stream->end_of_stream  = false;
            }
        } else {
            serial_write_str("[STREAMFILE] ERROR: adi_parse_header failed\n");
        }
    }

    return stream;
}

uint8_t* readstream(audio_datastream_t* stream, uint32_t* bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!stream || stream->write_mode || stream->end_of_stream) return NULL;
    if (!stream->file_handle || !stream->buffer)                 return NULL;

    if (stream->current_offset >= stream->total_size) {
        stream->end_of_stream = true;
        return NULL;
    }

    uint32_t remaining = stream->total_size - stream->current_offset;
    uint32_t to_read   = (remaining < stream->chunk_size) ?
                          remaining : stream->chunk_size;

    uint32_t file_pos = stream->data_offset + stream->current_offset;
    minimafs_seek(stream->file_handle, file_pos);

    uint32_t actually_read =
        minimafs_read(stream->file_handle, stream->buffer, to_read);

    if (actually_read == 0) {
        stream->end_of_stream = true;
        return NULL;
    }

    stream->current_offset += actually_read;
    if (bytes_read) *bytes_read = actually_read;
    return stream->buffer;
}

bool writestream(audio_datastream_t* stream, const uint8_t* data, uint32_t size) {
    if (!stream || !stream->write_mode || !data) return false;
    if (!stream->file_handle) return false;
    minimafs_seek(stream->file_handle, stream->current_offset);
    uint32_t written = minimafs_write(stream->file_handle, data, size);
    if (written != size) { serial_write_str("writestream: Write failed\n"); return false; }
    stream->current_offset += written;
    stream->total_size      = stream->current_offset;
    return true;
}

bool updatestream(audio_datastream_t* stream) {
    if (!stream || stream->end_of_stream) return false;
    stream->current_offset += stream->chunk_size;
    if (stream->current_offset >= stream->total_size) {
        stream->end_of_stream = true;
        return false;
    }
    return true;
}

bool seekstream(audio_datastream_t* stream, uint32_t offset) {
    if (!stream) return false;
    if (offset >= stream->total_size) {
        stream->current_offset = stream->total_size;
        stream->end_of_stream  = true;
        return true;
    }
    stream->current_offset = offset;
    stream->end_of_stream  = false;
    minimafs_seek(stream->file_handle, stream->data_offset + offset);
    return true;
}

uint8_t stream_progress(audio_datastream_t* stream) {
    if (!stream || stream->total_size == 0) return 0;
    uint32_t pos = stream->current_offset;
    if (pos > stream->total_size) pos = stream->total_size;
    return (uint8_t)((pos * 100) / stream->total_size);
}

void closestream(audio_datastream_t* stream) {
    if (!stream) return;
    if (stream->file_handle)  minimafs_close(stream->file_handle);
    if (stream->buffer)       free_mem(stream->buffer);
    if (stream->prefetch_buf) free_mem(stream->prefetch_buf);
    free_mem(stream);
}

/* ============================================================
 * ADI header parsing / writing
 * ============================================================ */

const char* adi_format_name(audio_format_t format) {
    switch (format) {
        case AUDIO_FORMAT_IMA_ADPCM: return "IMA-ADPCM";
        case AUDIO_FORMAT_MS_ADPCM:  return "MS-ADPCM";
        case AUDIO_FORMAT_FLAC:      return "FLAC";
        case AUDIO_FORMAT_PCM16:     return "PCM16";
        default:                     return "UNKNOWN";
    }
}

static audio_format_t parse_audio_format(const char* str) {
    if (strcmp(str, "IADPCM") == 0 || strcmp(str, "IMA-ADPCM") == 0)
        return AUDIO_FORMAT_IMA_ADPCM;
    if (strcmp(str, "MSADPCM") == 0 || strcmp(str, "MS-ADPCM") == 0)
        return AUDIO_FORMAT_MS_ADPCM;
    if (strcmp(str, "FLAC") == 0)  return AUDIO_FORMAT_FLAC;
    if (strcmp(str, "PCM16") == 0) return AUDIO_FORMAT_PCM16;
    return AUDIO_FORMAT_NONE;
}

bool adi_parse_header_from_stream(audio_datastream_t* stream, adi_header_t* header) {
    if (!stream || !header || !stream->file_handle) return false;

    const uint32_t HEADER_SIZE = 2048;
    char* header_buf = (char*)alloc(HEADER_SIZE);
    if (!header_buf) return false;

    minimafs_seek(stream->file_handle, 0);
    uint32_t read_bytes =
        minimafs_read(stream->file_handle, header_buf, HEADER_SIZE);

    if (read_bytes < 10) { free_mem(header_buf); return false; }

    memset(header, 0, sizeof(adi_header_t));
    /* Sensible defaults */
    header->sample_rate   = 48000;
    header->channels      = 2;
    header->global_volume = 80;
    header->format        = AUDIO_FORMAT_IMA_ADPCM;

    const char* line   = header_buf;
    uint32_t    offset = 0;

    while (offset < read_bytes) {
        const char* end = line;
        while (end < header_buf + read_bytes && *end && *end != '\n') end++;

        /* #DATA marker — everything after this is audio payload */
        if ((end - line) >= 5 &&
            (strncmp(line, "#DATA", 5) == 0 || strncmp(line, "DATA", 4) == 0)) {
            header->data_offset = (uint32_t)((end - header_buf) + 1);
            free_mem(header_buf);
            return true;
        }

        /* Parse key=value lines */
        const char* equals = line;
        while (equals < end && *equals != '=') equals++;

        if (equals < end && *equals == '=') {
            char key[32] = {0};
            char value[64] = {0};

            uint32_t key_len = (uint32_t)(equals - line);
            if (key_len > 0 && key_len < sizeof(key)) {
                strncpy(key, line, key_len);
                audio_trim_ascii(key);
            }

            const char* val_start = equals + 1;
            uint32_t    val_len   = (uint32_t)(end - val_start);
            if (val_len > 0 && val_len < sizeof(value)) {
                strncpy(value, val_start, val_len);
                audio_trim_ascii(value);
            }

            if (strcmp(key, "AudioFormat") == 0) {
                if (strstr(value, "IADPCM") || strstr(value, "IMA"))
                    header->format = AUDIO_FORMAT_IMA_ADPCM;
                else if (strstr(value, "PCM16"))
                    header->format = AUDIO_FORMAT_PCM16;
                else if (strstr(value, "MS-ADPCM"))
                    header->format = AUDIO_FORMAT_MS_ADPCM;
            } else if (strcmp(key, "SampleRate") == 0) {
                header->sample_rate = (uint32_t)parse_int(value);
            } else if (strcmp(key, "Channels") == 0) {
                header->channels = (uint8_t)parse_int(value);
            } else if (strcmp(key, "Globalvol") == 0) {
                header->global_volume = (uint8_t)parse_int(value);
            } else if (strcmp(key, "AudioLength") == 0) {
                header->length_seconds = (uint32_t)parse_int(value);
            } else if (strcmp(key, "AudioDatalen") == 0) {
                header->data_length = (uint32_t)parse_int(value);
            }
        }

        line   = (*end == '\n') ? end + 1 : end;
        offset = (uint32_t)(line - header_buf);
    }

    free_mem(header_buf);
    serial_write_str("ADI: No #DATA marker found\n");
    return false;
}

bool adi_write_header(const char* path, const adi_header_t* header) {
    if (!path || !header) return false;
    minimafs_file_handle_t* f = minimafs_open(path, false);
    if (!f) return false;

    char buf[1024];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off,
                    "#Audio data generated by MinimalOS\n");
    off += snprintf(buf + off, sizeof(buf) - off,
                    "AudioFormat=%s\n", adi_format_name(header->format));
    off += snprintf(buf + off, sizeof(buf) - off,
                    "AudioLength=%u\n",  header->length_seconds);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "AudioDatalen=%u\n", header->data_length);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "Globalvol=%u\n",    header->global_volume);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "SampleRate=%u\n",   header->sample_rate);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "Channels=%u\n",     header->channels);
    off += snprintf(buf + off, sizeof(buf) - off, "#DATA\n");

    minimafs_seek(f, 0);
    minimafs_write(f, buf, off);
    minimafs_close(f);
    return true;
}

/* ============================================================
 * IMA-ADPCM decoder
 * ============================================================ */

static const int ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static inline int16_t ima_decode_nibble(int code, int* predictor, int* step_index) {
    int step = ima_step_table[*step_index];
    int diff  = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    if (code & 8) diff  = -diff;
    *predictor += diff;
    if (*predictor >  32767) *predictor =  32767;
    if (*predictor < -32768) *predictor = -32768;
    *step_index += ima_index_table[code & 0xF];
    if (*step_index < 0)  *step_index = 0;
    if (*step_index > 88) *step_index = 88;
    return (int16_t)*predictor;
}

bool decode_ima_adpcm(
    const uint8_t* input,  uint32_t input_size,
    int16_t*       output, uint32_t* output_size,
    uint32_t       max_output,
    int* pred_l, int* step_l,
    int* pred_r, int* step_r)
{
    if (!input || !output || !output_size ||
        !pred_l || !step_l || !pred_r || !step_r) {
        return false;
    }

    uint32_t in_pos  = 0;
    uint32_t out_pos = 0;

    /*
     * Stereo interleaved layout:
     *   byte 0 = left  byte (two 4-bit nibbles: lo=sample0, hi=sample1)
     *   byte 1 = right byte (same)
     *   byte 2 = left  ...
     *   byte 3 = right ...
     *
     * Each pair of input bytes → 4 output samples: L0,R0,L1,R1
     */
    while (in_pos + 1 < input_size) {
        /* We are about to write 4 samples; check capacity first */
        if (out_pos + 4 > max_output) break;

        uint8_t lb = input[in_pos++];
        uint8_t rb = input[in_pos++];

        int16_t l0 = ima_decode_nibble((lb)      & 0x0F, pred_l, step_l);
        int16_t r0 = ima_decode_nibble((rb)      & 0x0F, pred_r, step_r);
        int16_t l1 = ima_decode_nibble((lb >> 4) & 0x0F, pred_l, step_l);
        int16_t r1 = ima_decode_nibble((rb >> 4) & 0x0F, pred_r, step_r);

        output[out_pos++] = l0;
        output[out_pos++] = r0;
        output[out_pos++] = l1;
        output[out_pos++] = r1;
    }

    *output_size = out_pos;
    return (out_pos > 0);
}

/* ============================================================
 * MS-ADPCM stub
 * ============================================================ */
bool decode_ms_adpcm(const uint8_t* input, uint32_t input_size,
                     int16_t* output, uint32_t* output_size) {
    (void)input; (void)input_size; (void)output;
    if (output_size) *output_size = 0;
    serial_write_str("MS-ADPCM decoder not implemented\n");
    return false;
}

/* ============================================================
 * FLAC stub
 * ============================================================ */
bool decode_flac(const uint8_t* input, uint32_t input_size,
                 int16_t* output, uint32_t* output_size) {
    (void)input; (void)input_size; (void)output;
    if (output_size) *output_size = 0;
    serial_write_str("FLAC decoder not implemented\n");
    return false;
}

/* ============================================================
 * Audio player
 * ============================================================ */

audio_player_t* audio_player_create(const char* path) {
    serial_write_str("[PLAYER] Creating player for: ");
    serial_write_str(path ? path : "(null)");
    serial_write_str("\n");

    if (!path) return NULL;

    audio_player_t* player =
        (audio_player_t*)alloc(sizeof(audio_player_t));
    if (!player) return NULL;
    memset(player, 0, sizeof(audio_player_t));

    /*
     * Compute the correct chunk size BEFORE opening the stream.
     *
     * IMA-ADPCM stereo: 2 compressed bytes → 4 PCM samples (L0,R0,L1,R1).
     * We want to fill exactly ONE half-buffer (pcm_capacity samples) per
     * readstream() call.
     *
     *   pcm_capacity   = AUDIO_BUFFER_SIZE * channels
     *   compressed_per_fill = pcm_capacity / 2
     *                       = (AUDIO_BUFFER_SIZE * channels) / 2
     *
     * For PCM16 the ratio is 1:2 (bytes:samples), so the chunk is also
     * pcm_capacity * sizeof(int16_t) / 2  =  pcm_capacity bytes per fill.
     * We use the same formula for simplicity; the decoder handles any
     * residual via the max_output guard.
     *
     * We open streamfile here with a placeholder chunk_size=4; it gets
     * updated below once we know the real channels from the header.
     * Actually we need the header first — so we open with a small chunk,
     * read the header, then re-open with the right chunk size.
     */

    /* First pass: open with small chunk just to parse the header */
    audio_datastream_t* probe =
        streamfile(path, false, 512, true);
    if (!probe) {
        serial_write_str("[PLAYER] streamfile probe failed\n");
        free_mem(player);
        return NULL;
    }
    if (!probe->is_adi) {
        serial_write_str("[PLAYER] Not an ADI file\n");
        closestream(probe);
        free_mem(player);
        return NULL;
    }

    /* Copy header info */
    adi_header_t hdr = probe->adi_header;
    closestream(probe);

    /* Clamp / validate */
    if (hdr.sample_rate == 0)  hdr.sample_rate = 48000;
    if (hdr.channels    == 0)  hdr.channels    = 2;
    if (hdr.channels    >  2)  hdr.channels    = 2;
    if (hdr.sample_rate > 48000) hdr.sample_rate = 48000;
    if (hdr.sample_rate <  8000) hdr.sample_rate =  8000;

    /*
     * pcm_capacity = samples in ONE half of the double-buffer.
     * This is the number of interleaved samples (L+R counted separately).
     */
    uint32_t channels     = hdr.channels;
    uint32_t pcm_capacity = AUDIO_BUFFER_SIZE * channels;

    /*
     * chunk_size_bytes = compressed bytes needed to produce pcm_capacity
     * PCM samples.
     *
     * IMA-ADPCM stereo:  2 bytes → 4 samples  →  ratio = 0.5 bytes/sample
     *   chunk = pcm_capacity * 0.5 = pcm_capacity / 2
     *
     * IMA-ADPCM mono:    1 byte  → 2 samples  →  ratio = 0.5 bytes/sample
     *   chunk = pcm_capacity / 2
     *
     * PCM16 stereo:      4 bytes → 2 samples  →  ratio = 2 bytes/sample
     *   chunk = pcm_capacity * 2
     *
     * We choose based on format.
     */
    uint32_t chunk_size_bytes;
    switch (hdr.format) {
        case AUDIO_FORMAT_IMA_ADPCM:
        case AUDIO_FORMAT_MS_ADPCM:
            /* 2 compressed bytes → 4 PCM samples (stereo) or 2 (mono) */
            chunk_size_bytes = pcm_capacity / 2;
            break;
        case AUDIO_FORMAT_PCM16:
            chunk_size_bytes = pcm_capacity * (uint32_t)sizeof(int16_t);
            break;
        default:
            chunk_size_bytes = pcm_capacity / 2;
            break;
    }

    /* Enforce a minimum to avoid degenerate chunk sizes */
    if (chunk_size_bytes < 64)    chunk_size_bytes = 64;
    /* Align to 2 bytes (stereo ADPCM pairs) */
    chunk_size_bytes &= ~1u;

    serial_write_str("[PLAYER] pcm_capacity=");
    serial_write_dec(pcm_capacity);
    serial_write_str(" chunk_bytes=");
    serial_write_dec(chunk_size_bytes);
    serial_write_str(" fmt=");
    serial_write_dec(hdr.format);
    serial_write_str(" rate=");
    serial_write_dec(hdr.sample_rate);
    serial_write_str(" ch=");
    serial_write_dec(channels);
    serial_write_str("\n");

    /* Re-open stream with the correct chunk size */
    player->stream = streamfile(path, false, chunk_size_bytes, true);
    if (!player->stream) {
        serial_write_str("[PLAYER] streamfile failed\n");
        free_mem(player);
        return NULL;
    }

    player->format        = hdr.format;
    player->volume        = hdr.global_volume;
    player->pcm_capacity  = pcm_capacity;

    /* Double-buffer: two halves of pcm_capacity samples each */
    uint32_t total_samples = pcm_capacity * 2;
    uint32_t buffer_bytes  = total_samples * (uint32_t)sizeof(int16_t);

    player->pcm_buffer = (int16_t*)alloc(buffer_bytes);
    if (!player->pcm_buffer) {
        serial_write_str("[PLAYER] PCM alloc failed\n");
        closestream(player->stream);
        free_mem(player);
        return NULL;
    }
    memset(player->pcm_buffer, 0, buffer_bytes);

    player->pcm_position   = 0;
    player->pcm_size       = 0;
    player->current_buffer = 0;
    player->playing        = false;
    player->loop           = false;
    player->adpcm_pred_l   = 0;
    player->adpcm_pred_r   = 0;
    player->adpcm_step_l   = 0;
    player->adpcm_step_r   = 0;

    serial_write_str("[PLAYER] Created OK\n");
    return player;
}

void audio_player_play(audio_player_t* player) {
    if (!player) return;
    if (!player->stream || !player->pcm_buffer) {
        serial_write_str("[PLAYER] play: invalid state\n");
        return;
    }

    g_audio_state.player  = player;
    g_audio_state.playing = false;

    /* Set sample rate BEFORE priming buffers */
    if (player->stream->adi_header.sample_rate != 0) {
        ac97_set_sample_rate(player->stream->adi_header.sample_rate);
    }

    /* Prefill front buffer (index 0) */
    serial_write_str("[PLAYER] Prefilling buffer 0...\n");
    uint32_t samples = audio_player_fill_buffer(player, 0);
    if (samples == 0) {
        serial_write_str("[PLAYER] Prefill failed — no samples decoded\n");
        g_audio_state.player = NULL;
        return;
    }

    player->current_buffer = 0;
    player->pcm_position   = 0;
    player->pcm_size       = samples;

    serial_write_str("[PLAYER] Prefill OK, samples=");
    serial_write_dec(samples);
    serial_write_str("\n");

    /* Expose player to mixer BEFORE starting DMA */
    g_audio_state.playing = true;
    player->playing        = true;

    /* Start DMA — this primes all 32 HW buffers from the player */
    ac97_start();

    serial_write_str("[PLAYER] Playback started\n");
}

void audio_player_pause(audio_player_t* player) {
    if (!player) return;
    player->playing        = false;
    g_audio_state.playing  = false;
    serial_write_str("[PLAYER] Paused\n");
}

void audio_player_stop(audio_player_t* player) {
    if (!player) return;
    player->playing        = false;
    g_audio_state.playing  = false;
    g_audio_state.player   = NULL;
    player->pcm_position   = 0;
    player->pcm_size       = 0;
    player->current_buffer = 0;
    if (player->stream) seekstream(player->stream, 0);
    serial_write_str("[PLAYER] Stopped\n");
}

void audio_player_set_volume(audio_player_t* player, uint8_t volume) {
    if (!player) return;
    if (volume > 100) volume = 100;
    player->volume = volume;
}

bool audio_player_update(audio_player_t* player) {
    if (!player) return false;
    if (!g_audio_state.playing || g_audio_state.player != player) return false;
    if (!player->playing) return false;

    if (player->pcm_position >= player->pcm_size) {
        uint8_t  next_buf = player->current_buffer ^ 1;
        uint32_t samples  = audio_player_fill_buffer(player, next_buf);
        if (samples == 0) {
            g_audio_state.playing = false;
            player->playing        = false;
            g_audio_state.player   = NULL;
            return false;
        }
        player->current_buffer = next_buf;
        player->pcm_position   = 0;
        player->pcm_size       = samples;
    }

    return true;
}

uint8_t audio_player_get_progress(audio_player_t* player) {
    if (!player || !player->stream) return 0;
    return stream_progress(player->stream);
}

void audio_player_destroy(audio_player_t* player) {
    if (!player) return;
    if (player == g_audio_state.player) {
        g_audio_state.player  = NULL;
        g_audio_state.playing = false;
    }
    if (player->stream)     closestream(player->stream);
    if (player->pcm_buffer) free_mem(player->pcm_buffer);
    free_mem(player);
    serial_write_str("[PLAYER] Destroyed\n");
}

/* ============================================================
 * Commands
 * ============================================================ */

void cmd_play(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: play <path>\n");
        graphics_write_textr("Example: play 0:/music.adi\n");
        return;
    }

    if (g_audio_state.player) {
        audio_player_destroy(g_audio_state.player);
        g_audio_state.player = NULL;
    }

    const char* path = argv[1];
    graphics_write_textr("Loading: ");
    graphics_write_textr(path);
    graphics_write_textr("\n");

    g_audio_state.player = audio_player_create(path);
    if (!g_audio_state.player) {
        graphics_write_textr("Failed to load audio file\n");
        return;
    }

    adi_header_t* h = &g_audio_state.player->stream->adi_header;
    char buf[64];

    graphics_write_textr("Format: ");
    graphics_write_textr(adi_format_name(h->format));
    graphics_write_textr("\n");

    snprintf(buf, sizeof(buf), "Duration: %u seconds\n", h->length_seconds);
    graphics_write_textr(buf);
    snprintf(buf, sizeof(buf), "Sample rate: %u Hz\n", h->sample_rate);
    graphics_write_textr(buf);
    snprintf(buf, sizeof(buf), "Channels: %u\n", (uint32_t)h->channels);
    graphics_write_textr(buf);
    snprintf(buf, sizeof(buf), "Volume: %u%%\n", (uint32_t)h->global_volume);
    graphics_write_textr(buf);

    graphics_write_textr("Playing...\n");
    audio_player_play(g_audio_state.player);
}

void cmd_stop(int argc, const char** argv) {
    if (g_audio_state.player) {
        audio_player_stop(g_audio_state.player);
        graphics_write_textr("Stopped\n");
    } else {
        graphics_write_textr("No audio playing\n");
    }
}

void cmd_pause(int argc, const char** argv) {
    if (g_audio_state.player) {
        if (g_audio_state.player->playing) {
            audio_player_pause(g_audio_state.player);
            graphics_write_textr("Paused\n");
        } else {
            audio_player_play(g_audio_state.player);
            graphics_write_textr("Resumed\n");
        }
    } else {
        graphics_write_textr("No audio playing\n");
    }
}

void cmd_volume(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: volume <0-100>\n");
        return;
    }
    if (!g_audio_state.player) {
        graphics_write_textr("No audio playing\n");
        return;
    }
    int vol = parse_int(argv[1]);
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    audio_player_set_volume(g_audio_state.player, (uint8_t)vol);
    char buf[32];
    snprintf(buf, sizeof(buf), "Volume: %d%%\n", vol);
    graphics_write_textr(buf);
}

void register_audio_commands(void) {
    command_register("play",   cmd_play);
    command_register("stop",   cmd_stop);
    command_register("pause",  cmd_pause);
    command_register("volume", cmd_volume);
}

REGISTER_COMMAND(register_audio_commands);
