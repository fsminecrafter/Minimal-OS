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

// ===========================================
// AUDIO HARDWARE BRIDGE
// ===========================================

static int16_t audio_apply_volume(int16_t sample, uint8_t volume) {
    if (volume >= 100) return sample;
    return (int16_t)(((int32_t)sample * volume) / 100);
}

static uint32_t audio_min_u32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static const uint8_t* audio_find_bytes(const uint8_t* haystack, uint32_t haystack_len, const char* needle) {
    if (!haystack || !needle) return NULL;

    uint32_t needle_len = (uint32_t)strlen(needle);
    if (needle_len == 0 || haystack_len < needle_len) return NULL;

    for (uint32_t i = 0; i <= haystack_len - needle_len; i++) {
        uint32_t j = 0;
        for (; j < needle_len; j++) {
            if (haystack[i + j] != (uint8_t)needle[j]) break;
        }
        if (j == needle_len) {
            return haystack + i;
        }
    }

    return NULL;
}

static void audio_trim_ascii(char* s) {
    if (!s) return;

    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        s[len - 1] = '\0';
        len--;
    }
}

static uint32_t audio_player_fill_buffer(audio_player_t* player, bool front_buffer) {
    if (!player || !player->stream || !player->pcm_buffer) {
        serial_write_str("[AUDIO_DBG] fill_buffer: NULL player/stream/buffer\n");
        return 0;
    }

    if (player->pcm_capacity == 0) {
        serial_write_str("[AUDIO_DBG] fill_buffer: capacity is zero\n");
        return 0;
    }

    int16_t* target_buffer = player->pcm_buffer +
        (front_buffer ? 0 : player->pcm_capacity);

    uint32_t bytes_read = 0;

    serial_write_str("[FILL] pre-read: pfetch_sz=");
    serial_write_dec(player->stream->prefetch_size);
    serial_write_str(" pfetch_pos=");
    serial_write_dec(player->stream->prefetch_pos);
    serial_write_str(" cur_off=");
    serial_write_dec(player->stream->current_offset);
    serial_write_str(" total=");
    serial_write_dec(player->stream->total_size);
    serial_write_str(" data_off=");
    serial_write_dec(player->stream->data_offset);
    serial_write_str("\n");

    uint8_t* compressed = readstream(player->stream, &bytes_read);

    serial_write_str("[FILL] post-read: bytes=");
    serial_write_dec(bytes_read);
    serial_write_str(" buf=");
    serial_write_dec((uint32_t)(uintptr_t)compressed);
    serial_write_str("\n");

    if (!compressed || bytes_read == 0) {
        if (player->loop) {
            seekstream(player->stream, 0);
            compressed = readstream(player->stream, &bytes_read);
        }

        if (!compressed || bytes_read == 0) {
            serial_write_str("[AUDIO_DBG] fill_buffer: no data\n");
            g_audio_state.playing = false;
            return 0;
        }
    }

    serial_write_str("[FILL] pre-decode: fmt=");
    serial_write_dec(player->format);
    serial_write_str(" bytes=");
    serial_write_dec(bytes_read);
    serial_write_str(" target=");
    serial_write_dec((uint32_t)(uintptr_t)target_buffer);
    serial_write_str(" pcm_buf=");
    serial_write_dec((uint32_t)(uintptr_t)player->pcm_buffer);
    serial_write_str(" cap=");
    serial_write_dec(player->pcm_capacity);
    serial_write_str(" front=");
    serial_write_dec((uint32_t)front_buffer);
    serial_write_str("\n");

    uint32_t decoded_samples = 0;
    bool success = false;

    switch (player->format) {
        case AUDIO_FORMAT_IMA_ADPCM:
            serial_write_str("[FILL] entering decode_ima_adpcm...\n");
            success = decode_ima_adpcm(compressed, bytes_read,
                                       target_buffer,
                                       &decoded_samples,
                                       player->pcm_capacity);
            serial_write_str("[FILL] decode_ima_adpcm returned success=");
            serial_write_dec((uint32_t)success);
            serial_write_str(" decoded=");
            serial_write_dec(decoded_samples);
            serial_write_str("\n");
            break;

        case AUDIO_FORMAT_PCM16: {
            uint32_t max_bytes = player->pcm_capacity * sizeof(int16_t);
            uint32_t copy = audio_min_u32(bytes_read, max_bytes);
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
        serial_write_str("[AUDIO_DBG] fill_buffer: decode failed\n");
        g_audio_state.playing = false;
        return 0;
    }

    player->pcm_position = 0;
    player->pcm_size = decoded_samples;
    return decoded_samples;
}

void audio_init(void) {
    ac97_init();
}

void audio_update(void) {
    ac97_update();

    if (g_audio_state.playing && g_audio_state.player) {
        audio_player_update(g_audio_state.player);
    }
}

void audio_mix_streams(int16_t* out, uint32_t frames) {
    static uint32_t dbg_counter = 0;
    static uint32_t last_report = 0;
    static bool last_had_player = false;
    static bool last_buffer_null = false;
    static bool last_exhausted = false;

    dbg_counter++;

    if (!out || frames == 0) {
        if (dbg_counter - last_report > 5000) {
            serial_write_str("[AUDIO] mix: invalid args\n");
            last_report = dbg_counter;
        }
        return;
    }
    
    uint32_t out_samples = frames * AUDIO_CHANNELS;
    audio_player_t* player = g_audio_state.player;

    // --- No player / not playing ---
    if (!player || !g_audio_state.playing) {
        memset(out, 0, out_samples * sizeof(int16_t));

        if (last_had_player) {
            serial_write_str("[AUDIO] stopped / no player\n");
        }
        last_had_player = false;

        return;
    }
    last_had_player = true;

    uint8_t src_channels = player->stream ? player->stream->adi_header.channels : AUDIO_CHANNELS;
    if (src_channels == 0) src_channels = AUDIO_CHANNELS;

    uint8_t out_channels = AUDIO_CHANNELS;
    uint8_t volume = player->volume;
    if (volume > 100) volume = 100;

    if (dbg_counter <= 3) {
        serial_write_str("[MIX] first mix: player=");
        serial_write_dec((uint32_t)(uintptr_t)player);
        serial_write_str(" pcm_buf=");
        serial_write_dec((uint32_t)(uintptr_t)player->pcm_buffer);
        serial_write_str(" pcm_pos=");
        serial_write_dec(player->pcm_position);
        serial_write_str(" pcm_size=");
        serial_write_dec(player->pcm_size);
        serial_write_str(" pcm_cap=");
        serial_write_dec(player->pcm_capacity);
        serial_write_str(" cur_buf=");
        serial_write_dec(player->current_buffer);
        serial_write_str(" src_ch=");
        serial_write_dec(src_channels);
        serial_write_str(" out=");
        serial_write_dec((uint32_t)(uintptr_t)out);
        serial_write_str(" frames=");
        serial_write_dec(frames);
        serial_write_str("\n");
    }

    // --- Buffer NULL check ---
    if (!player->pcm_buffer) {
        memset(out, 0, out_samples * sizeof(int16_t));

        if (!last_buffer_null) {
            serial_write_str("[AUDIO] ERROR: pcm_buffer NULL\n");
            last_buffer_null = true;
        }
        return;
    }
    last_buffer_null = false;

    int16_t* current_buffer = player->pcm_buffer +
        (player->current_buffer * player->pcm_capacity);

    uint32_t out_idx = 0;

    while (out_idx < out_samples) {
        // --- Buffer exhausted ---
        if (player->pcm_position >= player->pcm_size) {
            // Try to refill immediately instead of zero-filling
            bool ok = audio_player_update(player);
            if (!ok || player->pcm_size == 0) {
                // Truly out of data, zero fill remainder
                memset(out + out_idx, 0, (out_samples - out_idx) * sizeof(int16_t));
                return;
            }
            // Update current_buffer pointer after refill
            current_buffer = player->pcm_buffer +
                (player->current_buffer * player->pcm_capacity);
            continue;
        }
        last_exhausted = false;

        if (src_channels <= 1) {
            int16_t s = current_buffer[player->pcm_position++];
            s = audio_apply_volume(s, volume);

            if (out_channels == 1) {
                out[out_idx++] = s;
            } else {
                out[out_idx++] = s;
                if (out_idx < out_samples) out[out_idx++] = s;
            }
        } else {
            if (player->pcm_position + 1 >= player->pcm_size) {
                memset(out + out_idx, 0, (out_samples - out_idx) * sizeof(int16_t));
                return;
            }

            int16_t l = current_buffer[player->pcm_position++];
            int16_t r = current_buffer[player->pcm_position++];

            if (src_channels > 2) {
                uint32_t skip = src_channels - 2;
                if (player->pcm_position + skip <= player->pcm_size) {
                    player->pcm_position += skip;
                } else {
                    player->pcm_position = player->pcm_size;
                }
            }

            l = audio_apply_volume(l, volume);
            r = audio_apply_volume(r, volume);

            if (out_channels == 1) {
                out[out_idx++] = (int16_t)(((int32_t)l + r) / 2);
            } else {
                out[out_idx++] = l;
                if (out_idx < out_samples) out[out_idx++] = r;
            }
        }
    }

    if ((dbg_counter % 10000) == 0) {
        serial_write_str("[AUDIO] ok pos=");
        serial_write_dec(player->pcm_position);
        serial_write_str("/");
        serial_write_dec(player->pcm_size);
        serial_write_str("\n");
    }
}

// ===========================================
// STRING UTILITIES
// ===========================================

int32_t parse_int(const char* str) {
    if (!str) return 0;
    
    int32_t result = 0;
    bool negative = false;
    
    // Skip whitespace
    while (*str == ' ' || *str == '\t') str++;
    
    // Check sign
    if (*str == '-') {
        negative = true;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Parse digits
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return negative ? -result : result;
}

// ===========================================
// DATA STREAMING
// ===========================================

// Forward declarations
bool adi_parse_header_from_stream(audio_datastream_t* stream, adi_header_t* header);

audio_datastream_t* streamfile(const char* path, bool write_mode,
                               uint32_t chunk_size, bool include_metadata) {
    if (!path || chunk_size == 0) return NULL;

    if (chunk_size > 1024 * 1024) {
        serial_write_str("[STREAMFILE_DBG] ERROR: chunk_size too large\n");
        return NULL;
    }

    audio_datastream_t* stream = (audio_datastream_t*)alloc(sizeof(audio_datastream_t));
    if (!stream) return NULL;

    memset(stream, 0, sizeof(audio_datastream_t));

    strncpy(stream->path, path, sizeof(stream->path) - 1);
    stream->write_mode = write_mode;
    stream->chunk_size = chunk_size;
    stream->include_metadata = include_metadata;
    stream->current_offset = 0;
    stream->end_of_stream = false;
    stream->data_offset = 0;
    stream->total_size = 0;

    stream->buffer = (uint8_t*)alloc(chunk_size);
    if (!stream->buffer) {
        free_mem(stream);
        return NULL;
    }

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

    if (write_mode) {
        return stream;
    }

    uint32_t file_size = stream->file_handle->data_size;
    stream->total_size = file_size;

    if (include_metadata) {
        if (adi_parse_header_from_stream(stream, &stream->adi_header)) {
            stream->is_adi = true;
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
                stream->total_size = payload_size;
                stream->current_offset = 0;
                stream->end_of_stream = false;
            }
        } else {
            serial_write_str("[STREAMFILE_DBG] ERROR: adi_parse_header failed\n");
        }
    }

    return stream;
}

uint8_t* readstream(audio_datastream_t* stream, uint32_t* bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!stream || stream->write_mode || stream->end_of_stream) return NULL;
    if (!stream->file_handle || !stream->buffer) {
        stream->end_of_stream = true;
        return NULL;
    }
    if (stream->current_offset >= stream->total_size) {
        stream->end_of_stream = true;
        return NULL;
    }

    if (stream->prefetch_buf && stream->prefetch_pos < stream->prefetch_size) {
        uint32_t avail = stream->prefetch_size - stream->prefetch_pos;
        uint32_t serve = (avail < stream->chunk_size) ? avail : stream->chunk_size;
        serial_write_str("[RSTR] serving prefetch: avail=");
        serial_write_dec(avail);
        serial_write_str(" serve=");
        serial_write_dec(serve);
        serial_write_str(" buf=");
        serial_write_dec((uint32_t)(uintptr_t)(stream->prefetch_buf + stream->prefetch_pos));
        serial_write_str(" dst=");
        serial_write_dec((uint32_t)(uintptr_t)stream->buffer);
        serial_write_str("\n");
        memcpy(stream->buffer, stream->prefetch_buf + stream->prefetch_pos, serve);
        stream->prefetch_pos += serve;
        stream->current_offset += serve;
        if (bytes_read) *bytes_read = serve;
        if (stream->current_offset >= stream->total_size) stream->end_of_stream = true;
        return stream->buffer;
    }

    uint32_t remaining = stream->total_size - stream->current_offset;
    uint32_t to_read = (remaining < stream->chunk_size) ? remaining : stream->chunk_size;
    if (to_read == 0) { stream->end_of_stream = true; return NULL; }

    // On first read: the file handle is sitting at scan_size (4096) bytes into the file,
    // because adi_parse_header_from_stream already consumed that. data_offset is within
    // those 4096 bytes. We need to discard (scan_size - data_offset) leftover header
    // bytes that were read but not consumed yet — but since minimafs_read already
    // advanced the file handle by scan_size, and data_offset < scan_size, the actual
    // audio data starts INSIDE the buffer that was already read.
    // The simplest fix: store the leftover tail of the header buffer and serve it first.

    // NO seek — just read sequentially
    uint32_t read = minimafs_read(stream->file_handle, stream->buffer, to_read);

    if (read == 0) { stream->end_of_stream = true; return NULL; }

    stream->current_offset += read;
    if (bytes_read) *bytes_read = read;
    if (stream->current_offset >= stream->total_size) stream->end_of_stream = true;

    return stream->buffer;
}

bool writestream(audio_datastream_t* stream, const uint8_t* data, uint32_t size) {
    if (!stream || !stream->write_mode || !data) return false;
    
    if (!stream->file_handle) {
        // Create file if it doesn't exist
        // This would need minimafs_create_file implementation
        return false;
    }
    
    minimafs_seek(stream->file_handle, stream->current_offset);
    uint32_t written = minimafs_write(stream->file_handle, data, size);
    
    if (written != size) {
        serial_write_str("writestream: Write failed\n");
        return false;
    }
    
    stream->current_offset += written;
    stream->total_size = stream->current_offset;
    
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
        stream->end_of_stream = true;
        return true;
    }

    stream->current_offset = offset;
    stream->end_of_stream = false;

    // Re-position the file handle to match
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
    
    if (stream->file_handle) {
        minimafs_close(stream->file_handle);
    }
    
    if (stream->buffer) {
        free_mem(stream->buffer);
    }
    if (stream->prefetch_buf) free_mem(stream->prefetch_buf);
    free_mem(stream);
}


// ===========================================
// ADI FILE FORMAT
// ===========================================

const char* adi_format_name(audio_format_t format) {
    switch (format) {
        case AUDIO_FORMAT_IMA_ADPCM: return "IMA-ADPCM";
        case AUDIO_FORMAT_MS_ADPCM: return "MS-ADPCM";
        case AUDIO_FORMAT_FLAC: return "FLAC";
        case AUDIO_FORMAT_PCM16: return "PCM16";
        default: return "UNKNOWN";
    }
}

static audio_format_t parse_audio_format(const char* str) {
    if (strcmp(str, "IADPCM") == 0 || strcmp(str, "IMA-ADPCM") == 0) {
        return AUDIO_FORMAT_IMA_ADPCM;
    }
    if (strcmp(str, "MSADPCM") == 0 || strcmp(str, "MS-ADPCM") == 0) {
        return AUDIO_FORMAT_MS_ADPCM;
    }
    if (strcmp(str, "FLAC") == 0) {
        return AUDIO_FORMAT_FLAC;
    }
    if (strcmp(str, "PCM16") == 0) {
        return AUDIO_FORMAT_PCM16;
    }
    return AUDIO_FORMAT_NONE;
}

bool adi_parse_header_from_stream(audio_datastream_t* stream, adi_header_t* header) {
    if (!stream || !header || !stream->file_handle) return false;

    memset(header, 0, sizeof(adi_header_t));

    uint32_t old_offset = stream->current_offset;

    uint32_t scan_size = 4096;
    if (stream->file_handle->data_size > 0 && stream->file_handle->data_size < scan_size) {
        scan_size = stream->file_handle->data_size;
    }

    char* buffer = (char*)alloc(scan_size + 1);
    if (!buffer) return false;

    uint32_t read = minimafs_read(stream->file_handle, buffer, scan_size);

    if (read == 0) {
        free_mem(buffer);
        return false;
    }

    buffer[read] = '\0';

    const uint8_t* raw = (const uint8_t*)buffer;
    const uint8_t* search_start = raw;
    const uint8_t* search_end = raw + read;

    const uint8_t* data_marker = audio_find_bytes(search_start, read, "@DATA@");
    if (data_marker) {
        search_start = data_marker + 6;
    }

    const uint8_t* fmt_ptr = audio_find_bytes(search_start, (uint32_t)(search_end - search_start), "AudioFormat=");
    if (!fmt_ptr) {
        fmt_ptr = audio_find_bytes(raw, read, "AudioFormat=");
    }
    if (!fmt_ptr) {
        serial_write_str("ADI: missing AudioFormat\n");
        free_mem(buffer);
        return false;
    }

    const uint8_t* line_ptr = fmt_ptr;
    uint32_t data_start = 0;
    bool saw_data_marker = false;

    while (line_ptr < search_end) {
        const uint8_t* line_end = line_ptr;
        while (line_end < search_end && *line_end != '\n' && *line_end != '\r' && *line_end != '\0') {
            line_end++;
        }

        uint32_t line_len = (uint32_t)(line_end - line_ptr);
        if (line_len == 0) {
            if (*line_end == '\0') break;
            line_ptr = line_end + 1;
            continue;
        }

        char line[256];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, line_ptr, line_len);
        line[line_len] = '\0';
        audio_trim_ascii(line);

        if (strncmp(line, "AudioFormat=", 12) == 0) {
            char value[128];
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                audio_trim_ascii(value);
                header->format = parse_audio_format(value);
            }
        } else if (strncmp(line, "AudioLength=", 12) == 0) {
            char value[128];
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                audio_trim_ascii(value);
                header->length_seconds = (uint32_t)parse_int(value);
            }
        } else if (strncmp(line, "AudioDatalen=", 13) == 0) {
            char value[128];
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                audio_trim_ascii(value);
                header->data_length = (uint32_t)parse_int(value);
            }
        } else if (strncmp(line, "Globalvol=", 10) == 0) {
            char value[128];
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                audio_trim_ascii(value);
                header->global_volume = (uint8_t)parse_int(value);
            }
        } else if (strncmp(line, "SampleRate=", 11) == 0) {
            char value[128];
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                audio_trim_ascii(value);
                header->sample_rate = (uint32_t)parse_int(value);
            }
        } else if (strncmp(line, "Channels=", 9) == 0) {
            char value[128];
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                audio_trim_ascii(value);
                header->channels = (uint8_t)parse_int(value);
            }
        } else if (strcmp(line, "#DATA") == 0) {
            saw_data_marker = true;
            data_start = (uint32_t)((line_end < search_end) ? (line_end + 1 - raw) : read);
            while (data_start < read) {
                uint8_t c = raw[data_start];
                if (c != '\r' && c != '\n' && c != ' ' && c != '\t' && c != '\0') break;
                data_start++;
            }
            break;
        }

        if (*line_end == '\0') break;
        line_ptr = line_end + 1;
        while (line_ptr < search_end && (*line_ptr == '\r' || *line_ptr == '\n' || *line_ptr == '\0')) {
            line_ptr++;
        }
    }

    if (header->format == AUDIO_FORMAT_NONE) {
        serial_write_str("ADI: invalid format\n");
        free_mem(buffer);
        return false;
    }

    if (!saw_data_marker) {
        serial_write_str("ADI: missing #DATA marker\n");
        free_mem(buffer);
        return false;
    }

    // Set data_offset FIRST, then compute prefetch using it
    header->data_offset = data_start;

    // Save audio bytes already read past the header into prefetch buffer
    if (data_start < read) {
        uint32_t leftover = read - data_start;
        stream->prefetch_buf = (uint8_t*)alloc(leftover);
        if (stream->prefetch_buf) {
            memcpy(stream->prefetch_buf, buffer + data_start, leftover);
            stream->prefetch_size = leftover;
            stream->prefetch_pos = 0;
        }
    }

    free_mem(buffer);
    return true;
}

bool adi_write_header(const char* path, const adi_header_t* header) {
    if (!path || !header) return false;
    
    minimafs_file_handle_t* f = minimafs_open(path, false);
    if (!f) return false;
    
    // Build header string
    char header_str[1024];
    int offset = 0;
    
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "#Audio data generated by MinimalOS\n");
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "AudioFormat=%s\n", adi_format_name(header->format));
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "AudioLength=%u\n", header->length_seconds);
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "AudioDatalen=%u\n", header->data_length);
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "Globalvol=%u\n", header->global_volume);
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "SampleRate=%u\n", header->sample_rate);
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "Channels=%u\n", header->channels);
    offset += snprintf(header_str + offset, sizeof(header_str) - offset,
                      "#DATA\n");
    
    minimafs_seek(f, 0);
    minimafs_write(f, header_str, offset);
    minimafs_close(f);
    
    return true;
}

// ===========================================
// IMA ADPCM DECODER
// ===========================================

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

// Decode a single IMA-ADPCM nibble, updating predictor and step_index in place
static inline int16_t ima_decode_nibble(int code, int* predictor, int* step_index) {
    int step = ima_step_table[*step_index];
    int diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    if (code & 8) diff = -diff;
    *predictor += diff;
    if (*predictor >  32767) *predictor =  32767;
    if (*predictor < -32768) *predictor = -32768;
    *step_index += ima_index_table[code & 0xF];
    if (*step_index < 0)  *step_index = 0;
    if (*step_index > 88) *step_index = 88;
    return (int16_t)*predictor;
}

bool decode_ima_adpcm(const uint8_t* input, uint32_t input_size,
                      int16_t* output, uint32_t* output_size,
                      uint32_t max_output) {
    if (!input || !output || !output_size) return false;

    // Stereo IMA-ADPCM format:
    //   Each byte holds TWO samples for the SAME channel (lo nibble = sample N,
    //   hi nibble = sample N+1). Bytes ALTERNATE between left and right channels:
    //     Byte 0 (even) -> L[0], L[1]
    //     Byte 1 (odd)  -> R[0], R[1]
    //     Byte 2 (even) -> L[2], L[3]
    //     Byte 3 (odd)  -> R[2], R[3]  ...
    //
    // AC97 expects interleaved PCM: L[0], R[0], L[1], R[1], ...
    // So we decode pairs of bytes (one L byte + one R byte) and interleave.

    int pred_l = 0, step_l = 0;
    int pred_r = 0, step_r = 0;
    uint32_t out_pos = 0;

    // Process pairs of bytes: [L_byte, R_byte]
    uint32_t pairs = input_size / 2;
    for (uint32_t p = 0; p < pairs; p++) {
        uint8_t rb = input[p * 2 + 0];  // left byte
        uint8_t lb = input[p * 2 + 1];  // right byte

        // Decode two left samples and two right samples from this pair
        int16_t l0 = ima_decode_nibble(lb & 0x0F,        &pred_l, &step_l);
        int16_t l1 = ima_decode_nibble((lb >> 4) & 0x0F, &pred_l, &step_l);
        int16_t r0 = ima_decode_nibble(rb & 0x0F,        &pred_r, &step_r);
        int16_t r1 = ima_decode_nibble((rb >> 4) & 0x0F, &pred_r, &step_r);

        // Interleave: L0 R0 L1 R1
        if (out_pos + 3 >= max_output) break;
        output[out_pos++] = l0;
        output[out_pos++] = r0;
        output[out_pos++] = l1;
        output[out_pos++] = r1;
    }

    *output_size = out_pos;
    return true;
}

// ===========================================
// MS ADPCM DECODER (Simplified)
// ===========================================

bool decode_ms_adpcm(const uint8_t* input, uint32_t input_size,
                     int16_t* output, uint32_t* output_size) {
    if (!input || !output || !output_size) return false;
    
    // MS-ADPCM is more complex than IMA-ADPCM
    // For now, provide a simplified decoder
    // You'd need the full coefficient tables and state machine
    
    serial_write_str("MS-ADPCM decoder not fully implemented\n");
    *output_size = 0;
    return false;
}

// ===========================================
// FLAC DECODER (Stub)
// ===========================================

bool decode_flac(const uint8_t* input, uint32_t input_size,
                 int16_t* output, uint32_t* output_size) {
    if (!input || !output || !output_size) return false;
    
    // FLAC decoding requires a full library (libFLAC)
    // This is a stub - you'd need to integrate libFLAC or write a decoder
    
    serial_write_str("FLAC decoder not implemented (requires libFLAC)\n");
    *output_size = 0;
    return false;
}

// ===========================================
// AUDIO PLAYER
// ===========================================

audio_player_t* audio_player_create(const char* path) {
    serial_write_str("[PLAYER_CREATE_DBG] Starting...\n");

    if (!path) {
        serial_write_str("[PLAYER_CREATE_DBG] ERROR: NULL path\n");
        return NULL;
    }

    audio_player_t* player = (audio_player_t*)alloc(sizeof(audio_player_t));
    if (!player) return NULL;

    memset(player, 0, sizeof(audio_player_t));

    serial_write_str("[PLAYER_CREATE_DBG] Creating stream...\n");
    player->stream = streamfile(path, false, 4096, true);
    if (!player->stream) {
        serial_write_str("[PLAYER_CREATE_DBG] ERROR: streamfile returned NULL\n");
        free_mem(player);
        return NULL;
    }

    serial_write_str("[PLAYER_CREATE_DBG] Stream created, is_adi=");
    serial_write_str(player->stream->is_adi ? "yes" : "NO");
    serial_write_str("\n");

    if (!player->stream->is_adi) {
        serial_write_str("[PLAYER_CREATE_DBG] ERROR: Not an ADI file\n");
        closestream(player->stream);
        free_mem(player);
        return NULL;
    }

    player->format = player->stream->adi_header.format;
    player->volume = player->stream->adi_header.global_volume;

    uint32_t sample_rate = player->stream->adi_header.sample_rate;
    uint32_t channels = player->stream->adi_header.channels;

    if (sample_rate == 0) sample_rate = 48000;
    if (channels == 0) channels = 2;
    if (channels > 2) channels = 2;
    if (sample_rate > 48000) sample_rate = 48000;
    if (sample_rate < 8000) sample_rate = 8000;

    // pcm_capacity: 4 AC97 buffers worth of decoded PCM per channel.
    // Large enough that the main-loop refill (audio_update) keeps ahead of
    // the IRQ consumer (ac97_update), small enough to not exhaust the heap.
    // Each half of the double-buffer holds 4 * AUDIO_BUFFER_SIZE frames * channels.
    player->pcm_capacity = AUDIO_BUFFER_SIZE * 4 * channels;

    serial_write_str("[PLAYER_CREATE_DBG] Header: format=");
    serial_write_dec(player->format);
    serial_write_str(", rate=");
    serial_write_dec(sample_rate);
    serial_write_str(", ch=");
    serial_write_dec(channels);
    serial_write_str(", capacity=");
    serial_write_dec(player->pcm_capacity);
    serial_write_str("\n");

    uint32_t total_samples = player->pcm_capacity * 2;
    uint32_t buffer_bytes = total_samples * sizeof(int16_t);

    serial_write_str("[PLAYER_CREATE_DBG] Allocating double PCM buffer (");
    serial_write_dec(buffer_bytes);
    serial_write_str(" bytes for ");
    serial_write_dec(total_samples);
    serial_write_str(" samples)...\n");

    player->pcm_buffer = (int16_t*)alloc(buffer_bytes);
    if (!player->pcm_buffer) {
        serial_write_str("[PLAYER_CREATE_DBG] ERROR: Failed to allocate PCM buffer\n");
        closestream(player->stream);
        free_mem(player);
        return NULL;
    }

    memset(player->pcm_buffer, 0, buffer_bytes);

    player->pcm_position = 0;
    player->pcm_size = 0;
    player->current_buffer = 0;
    player->playing = false;
    player->loop = false;

    serial_write_str("[PLAYER_CREATE_DBG] SUCCESS\n");
    serial_write_str("Audio player created for: ");
    serial_write_str(path);
    serial_write_str(" (");
    serial_write_str(adi_format_name(player->format));
    serial_write_str(", ");
    serial_write_dec(sample_rate);
    serial_write_str("Hz, ");
    serial_write_dec(channels);
    serial_write_str(" ch, double-buffered)\n");

    return player;
}

void audio_player_play(audio_player_t* player) {
    if (!player) return;

    if (!player->stream || !player->pcm_buffer) {
        serial_write_str("[AUDIO] play: invalid player state\n");
        return;
    }

    g_audio_state.player = player;
    g_audio_state.playing = false;

    serial_write_str("[PLAY] calling prefill fill_buffer...\n");
    uint32_t samples = audio_player_fill_buffer(player, true);
    serial_write_str("[PLAY] prefill done, samples=");
    serial_write_dec(samples);
    serial_write_str("\n");

    if (samples == 0) {
        serial_write_str("[AUDIO] prefill failed\n");
        g_audio_state.player = NULL;
        return;
    }

    player->current_buffer = 0;
    player->pcm_position = 0;
    player->pcm_size = samples;

    serial_write_str("[PLAY] pcm_buffer=");
    serial_write_dec((uint32_t)(uintptr_t)player->pcm_buffer);
    serial_write_str(" pcm_capacity=");
    serial_write_dec(player->pcm_capacity);
    serial_write_str(" pcm_size=");
    serial_write_dec(player->pcm_size);
    serial_write_str("\n");

    if (player->stream->adi_header.sample_rate != 0) {
        ac97_set_sample_rate(player->stream->adi_header.sample_rate);
    }

    serial_write_str("[PLAY] setting playing=true, about to return\n");
    g_audio_state.playing = true;
    player->playing = true;

    serial_write_str("Audio playback started (double-buffered)\n");
}

void audio_player_pause(audio_player_t* player) {
    if (!player) return;
    player->playing = false;
    g_audio_state.playing = false;
    serial_write_str("Audio playback paused\n");
}

void audio_player_stop(audio_player_t* player) {
    if (!player) return;
    player->playing = false;
    g_audio_state.playing = false;
    g_audio_state.player = NULL;
    player->pcm_position = 0;
    player->pcm_size = 0;
    player->current_buffer = 0;
    if (player->stream) {
        seekstream(player->stream, 0);
    }
    serial_write_str("Audio playback stopped\n");
}

void audio_player_set_volume(audio_player_t* player, uint8_t volume) {
    if (!player) return;
    
    if (volume > 100) volume = 100;
    player->volume = volume;
}

bool audio_player_update(audio_player_t* player) {
    if (!player) return false;

    if (!g_audio_state.playing || g_audio_state.player != player) {
        return false;
    }

    if (!player->playing) {
        return false;
    }

    if (player->pcm_position >= player->pcm_size) {
        bool fill_front = (player->current_buffer == 1);

        uint32_t samples = audio_player_fill_buffer(player, fill_front);
        if (samples == 0) {
            g_audio_state.playing = false;
            player->playing = false;
            g_audio_state.player = NULL;
            return false;
        }

        player->current_buffer = fill_front ? 0 : 1;
        player->pcm_position = 0;
        player->pcm_size = samples;
    }

    return true;
}

uint8_t audio_player_get_progress(audio_player_t* player) {
    if (!player || !player->stream) return 0;
    
    return stream_progress(player->stream);
}

void audio_player_destroy(audio_player_t* player) {
    if (!player) return;
    
    if (player->stream) {
        closestream(player->stream);
    }
    
    if (player->pcm_buffer) {
        free_mem(player->pcm_buffer);
    }
    
    free_mem(player);
    
    serial_write_str("Audio player destroyed\n");
}

// ===========================================
// COMMAND: PLAY
// ===========================================

void cmd_play(int argc, const char** argv) {
    // NOTE: Do NOT call trace_enable(true) here.
    // trace_print() calls "sti; hlt; cli" every 200 lines which
    // corrupts the execution stack and crashes graphics calls.
    serial_write_str("\n=== CMD_PLAY START ===argc=");
    serial_write_dec(argc);
    serial_write_str("\n");

    if (argc < 2) {
        graphics_write_textr("Usage: play <path>\n");
        graphics_write_textr("Example: play 0:/music.adi\n");
        return;
    }

    if (g_audio_state.player) {
        serial_write_str("[CMD_PLAY] Destroying previous player\n");
        audio_player_destroy(g_audio_state.player);
        g_audio_state.player = NULL;
    }

    const char* path = argv[1];
    serial_write_str("[CMD_PLAY] Path: ");
    serial_write_str(path);
    serial_write_str("\n");

    graphics_write_textr("Loading: ");
    graphics_write_textr(path);
    graphics_write_textr("\n");

    serial_write_str("[CMD_PLAY] Creating audio player...\n");
    g_audio_state.player = audio_player_create(path);
    if (!g_audio_state.player) {
        serial_write_str("[CMD_PLAY] ERROR: Failed to create player\n");
        graphics_write_textr("Failed to load audio file!\n");
        return;
    }
    serial_write_str("[CMD_PLAY] Player created OK\n");

    // Log heap state so we can verify allocations aren't too large
    serial_write_str("[CMD_PLAY] Heap used=");
    serial_write_dec((uint32_t)allocator_used_bytes());
    serial_write_str(" free=");
    serial_write_dec((uint32_t)allocator_free_bytes());
    serial_write_str("\n");

    // Read all header fields into local variables BEFORE any graphics call.
    // Use the header pointer only here — once playback starts the player
    // could theoretically be freed by a stop command.
    audio_format_t  hdr_format  = g_audio_state.player->stream->adi_header.format;
    uint32_t        hdr_rate    = g_audio_state.player->stream->adi_header.sample_rate;
    uint8_t         hdr_ch      = g_audio_state.player->stream->adi_header.channels;
    uint8_t         hdr_vol     = g_audio_state.player->stream->adi_header.global_volume;
    uint32_t        hdr_dur     = g_audio_state.player->stream->adi_header.length_seconds;

    serial_write_str("[CMD_PLAY] hdr: fmt=");
    serial_write_dec(hdr_format);
    serial_write_str(" rate=");
    serial_write_dec(hdr_rate);
    serial_write_str(" ch=");
    serial_write_dec(hdr_ch);
    serial_write_str(" vol=");
    serial_write_dec(hdr_vol);
    serial_write_str(" dur=");
    serial_write_dec(hdr_dur);
    serial_write_str("\n");

    trace_dump_interrupt_audit();
    trace_dump_registers();
    char buf[64];
    serial_write_str("Hello1\n");
    graphics_write_textr("Format: ");
    serial_write_str("Hello2\n");
    //graphics_write_textr(adi_format_name(hdr_format));
    graphics_write_textr("\n");
    serial_write_str("Hello3\n");

    snprintf(buf, sizeof(buf), "Duration: %u seconds\n", hdr_dur);
    graphics_write_textr(buf);

    snprintf(buf, sizeof(buf), "Sample rate: %u Hz\n", hdr_rate);
    graphics_write_textr(buf);

    snprintf(buf, sizeof(buf), "Channels: %u\n", (uint32_t)hdr_ch);
    graphics_write_textr(buf);

    snprintf(buf, sizeof(buf), "Volume: %u%%\n", (uint32_t)hdr_vol);
    graphics_write_textr(buf);

    graphics_write_textr("Playing... (press 'p' to pause, 's' to stop)\n");

    serial_write_str("[CMD_PLAY] Starting playback\n");
    audio_player_play(g_audio_state.player);
    serial_write_str("[CMD_PLAY] Playback started OK\n");
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
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    
    audio_player_set_volume(g_audio_state.player, vol);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "Volume set to %d%%\n", vol);
    graphics_write_textr(buf);
}

void register_audio_commands(void) {
    command_register("play", cmd_play);
    command_register("stop", cmd_stop);
    command_register("pause", cmd_pause);
    command_register("volume", cmd_volume);
}

REGISTER_COMMAND(register_audio_commands);