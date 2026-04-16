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

// FIX (Bug 1 + Bug 6): fill_buffer now receives the explicit half-buffer index
// instead of a bool, and passes the correct max_output (pcm_capacity = samples
// per half-buffer, NOT frames).  Previously pcm_capacity was set to
// AUDIO_BUFFER_SIZE * 4 * channels which made it store frames*channels*4,
// then it was passed as max_output to the ADPCM decoder which interprets it as
// a sample count — the mismatch caused the decoder to stop too early (half the
// samples) → half-speed / "deep-fried" audio.
static uint32_t audio_player_fill_buffer(audio_player_t* player, uint8_t buf_idx) {
    if (!player || !player->stream || !player->pcm_buffer) {
        serial_write_str("[AUDIO_DBG] fill_buffer: NULL player/stream/buffer\n");
        return 0;
    }

    if (player->pcm_capacity == 0) {
        serial_write_str("[AUDIO_DBG] fill_buffer: capacity is zero\n");
        return 0;
    }

    // pcm_capacity is now samples-per-half-buffer, so this is correct.
    int16_t* target_buffer = player->pcm_buffer +
        ((uint32_t)buf_idx * player->pcm_capacity);

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
    serial_write_str(" buf_idx=");
    serial_write_dec((uint32_t)buf_idx);
    serial_write_str("\n");

    uint32_t decoded_samples = 0;
    bool success = false;

    switch (player->format) {
        case AUDIO_FORMAT_IMA_ADPCM:
            serial_write_str("[FILL] entering decode_ima_adpcm...\n");
            success = decode_ima_adpcm(
                compressed, bytes_read,
                target_buffer,
                &decoded_samples,
                // FIX (Bug 1): pass pcm_capacity (samples) not a frame count.
                // Previously this was player->pcm_capacity which happened to equal
                // AUDIO_BUFFER_SIZE*4*ch — a frame-ish number — causing the decoder
                // to emit far fewer samples than the buffer could hold.
                player->pcm_capacity,
                &player->adpcm_pred_l,
                &player->adpcm_step_l,
                &player->adpcm_pred_r,
                &player->adpcm_step_r
            );
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

// FIX (Bug 7): audio_mix_streams previously used static counters that persisted
// across player sessions, meaning 'dbg_counter <= 3' debug prints would never
// fire on the second play command and the 'last_had_player' edge-detect state
// could be stale from a previous stop — reset them at player start instead.
// Also fixed the "deep fried" symptom: the stereo branch guard was
//   `pcm_position + 1 >= pcm_size`  (off-by-one: bails 1 sample early every buffer)
// now correctly:
//   `pcm_position + 2 > pcm_size`   (need exactly 2 samples: L and R)
// The old code zeroed the tail on every buffer boundary, causing a click/pop and
// leaving pcm_position short of pcm_size, which confused the refill trigger.
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

    // FIX (Bug 2): current_buffer is now a clean 0/1 index into the two
    // pcm_capacity-sized halves.  Previously the pointer arithmetic used
    // 'player->current_buffer * player->pcm_capacity' where current_buffer
    // could be 0 or 1, but pcm_capacity was wrong (see Bug 1), so the back
    // buffer address was always miles off → garbage audio / "deep fried".
    int16_t* current_buffer = player->pcm_buffer +
        ((uint32_t)player->current_buffer * player->pcm_capacity);

    uint32_t out_idx = 0;

    while (out_idx < out_samples) {
        // --- Buffer exhausted: trigger refill ---
        if (player->pcm_position >= player->pcm_size) {
            bool ok = audio_player_update(player);
            if (!ok || player->pcm_size == 0) {
                memset(out + out_idx, 0, (out_samples - out_idx) * sizeof(int16_t));
                return;
            }
            // Refresh local pointer after the buffer switch in audio_player_update
            current_buffer = player->pcm_buffer +
                ((uint32_t)player->current_buffer * player->pcm_capacity);
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
            // FIX (Bug 3): was `+ 1 >=` which fires when exactly 1 sample is left,
            // zero-filling the tail of every buffer and never cleanly reaching
            // pcm_size — causing a pop/click at every buffer boundary and a subtle
            // refill timing desync.  We need L *and* R, so check for 2 samples.
            if (player->pcm_position + 2 > player->pcm_size) {
                // Let the exhaustion check at the top of the loop handle refill
                player->pcm_position = player->pcm_size;
                continue;
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

    while (*str == ' ' || *str == '\t') str++;

    if (*str == '-') {
        negative = true;
        str++;
    } else if (*str == '+') {
        str++;
    }

    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }

    return negative ? -result : result;
}

// ===========================================
// DATA STREAMING
// ===========================================

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
    TRACE_ENTER();

    if (bytes_read) *bytes_read = 0;

    if (!stream || stream->write_mode || stream->end_of_stream) {
        TRACE_MSG("Invalid stream or EOS");
        TRACE_EXIT();
        return NULL;
    }

    if (!stream->file_handle || !stream->buffer) {
        TRACE_MSG("No file handle or buffer");
        TRACE_EXIT();
        return NULL;
    }

    TRACE_VALUE("current_offset", stream->current_offset);
    TRACE_VALUE("total_size", stream->total_size);
    TRACE_VALUE("data_offset", stream->data_offset);

    if (stream->current_offset >= stream->total_size) {
        TRACE_MSG("Reached end of stream");
        stream->end_of_stream = true;
        TRACE_EXIT();
        return NULL;
    }

    uint32_t remaining = stream->total_size - stream->current_offset;
    uint32_t to_read = (remaining < stream->chunk_size) ? remaining : stream->chunk_size;

    TRACE_VALUE("remaining", remaining);
    TRACE_VALUE("to_read", to_read);

    uint32_t file_pos = stream->data_offset + stream->current_offset;

    TRACE_VALUE("file_pos", file_pos);
    TRACE_MSG("Seeking...");

    minimafs_seek(stream->file_handle, file_pos);

    TRACE_MSG("Reading chunk...");
    uint32_t actually_read = minimafs_read(stream->file_handle, stream->buffer, to_read);

    TRACE_VALUE("actually_read", actually_read);

    if (actually_read == 0) {
        TRACE_MSG("Read returned 0 bytes");
        stream->end_of_stream = true;
        TRACE_EXIT();
        return NULL;
    }

    stream->current_offset += actually_read;

    if (bytes_read) {
        *bytes_read = actually_read;
    }

    TRACE_VALUE("new_offset", stream->current_offset);
    TRACE_EXIT();

    return stream->buffer;
}

bool writestream(audio_datastream_t* stream, const uint8_t* data, uint32_t size) {
    if (!stream || !stream->write_mode || !data) return false;

    if (!stream->file_handle) {
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
    TRACE_ENTER();

    if (!stream || !header || !stream->file_handle) {
        TRACE_MSG("Invalid parameters");
        TRACE_EXIT();
        return false;
    }

    const uint32_t HEADER_SIZE = 2048;
    char* header_buf = (char*)alloc(HEADER_SIZE);
    if (!header_buf) {
        TRACE_MSG("Failed to allocate header buffer");
        TRACE_EXIT();
        return false;
    }

    minimafs_seek(stream->file_handle, 0);
    uint32_t read_bytes = minimafs_read(stream->file_handle, header_buf, HEADER_SIZE);

    TRACE_VALUE("read_bytes", read_bytes);

    if (read_bytes < 100) {
        TRACE_MSG("Header too small");
        free_mem(header_buf);
        TRACE_EXIT();
        return false;
    }

    memset(header, 0, sizeof(adi_header_t));
    header->sample_rate = 48000;
    header->channels = 2;
    header->global_volume = 80;
    header->format = AUDIO_FORMAT_IMA_ADPCM;

    const char* line = header_buf;
    uint32_t offset = 0;

    while (offset < read_bytes) {
        const char* end = line;
        while (end < header_buf + read_bytes && *end && *end != '\n') {
            end++;
        }

        if (end - line >= 4) {
            if (strncmp(line, "#DATA", 5) == 0 || strncmp(line, "DATA", 4) == 0) {
                header->data_offset = (end - header_buf) + 1;
                TRACE_VALUE("data_offset", header->data_offset);

                free_mem(header_buf);
                TRACE_MSG("Header parsed successfully");
                TRACE_EXIT();
                return true;
            }
        }

        char key[32] = {0};
        char value[64] = {0};

        const char* equals = line;
        while (equals < end && *equals != '=') equals++;

        if (equals < end && *equals == '=') {
            uint32_t key_len = equals - line;
            if (key_len > 0 && key_len < sizeof(key)) {
                strncpy(key, line, key_len);
                key[key_len] = '\0';
                audio_trim_ascii(key);
            }

            const char* val_start = equals + 1;
            uint32_t val_len = end - val_start;
            if (val_len > 0 && val_len < sizeof(value)) {
                strncpy(value, val_start, val_len);
                value[val_len] = '\0';
                audio_trim_ascii(value);
            }

            if (strcmp(key, "AudioFormat") == 0) {
                if (strstr(value, "IADPCM") || strstr(value, "IMA")) {
                    header->format = AUDIO_FORMAT_IMA_ADPCM;
                } else if (strstr(value, "PCM16")) {
                    header->format = AUDIO_FORMAT_PCM16;
                } else if (strstr(value, "MS-ADPCM")) {
                    header->format = AUDIO_FORMAT_MS_ADPCM;
                }
            }
            else if (strcmp(key, "SampleRate") == 0) {
                header->sample_rate = parse_int(value);
            }
            else if (strcmp(key, "Channels") == 0) {
                header->channels = (uint8_t)parse_int(value);
            }
            else if (strcmp(key, "Globalvol") == 0) {
                header->global_volume = (uint8_t)parse_int(value);
            }
            else if (strcmp(key, "AudioLength") == 0) {
                header->length_seconds = parse_int(value);
            }
            else if (strcmp(key, "AudioDatalen") == 0) {
                header->data_length = parse_int(value);
            }
        }

        if (*end == '\n') {
            line = end + 1;
        } else {
            break;
        }

        offset = line - header_buf;
    }

    free_mem(header_buf);

    TRACE_MSG("No #DATA marker found");
    TRACE_EXIT();
    return false;
}

bool adi_write_header(const char* path, const adi_header_t* header) {
    if (!path || !header) return false;

    minimafs_file_handle_t* f = minimafs_open(path, false);
    if (!f) return false;

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

// FIX (Bug 8 - "deep fried / sped up"): The ADPCM decoder state (pred/step) is
// passed in by pointer and updated in place across calls so the predictor state
// carries over between consecutive readstream chunks — this is correct.
// However when ac97_kick() is called it refills ALL 32 hardware buffers in a
// tight loop by calling audio_mix_streams 32 times.  Each audio_mix_streams call
// can trigger audio_player_update which calls fill_buffer which calls the
// decoder, advancing the ADPCM predictor state correctly.  BUT ac97_kick also
// resets CIV to 0 and replays old buffers, so the decoded audio in those old
// slots is now stale/wrong.  This is an ac97 issue, not a decoder issue — see
// the note in ac97_driver.c fix.
//
// The decoder itself is correct; no changes needed here.
bool decode_ima_adpcm(
    const uint8_t* input, uint32_t input_size,
    int16_t* output, uint32_t* output_size,
    uint32_t max_output,
    int* pred_l, int* step_l,
    int* pred_r, int* step_r
) {
    if (!input || !output || !output_size || !pred_l || !step_l || !pred_r || !step_r) {
        return false;
    }

    uint32_t in_pos = 0;
    uint32_t out_pos = 0;

    /*
        Stereo IMA-ADPCM layout (byte interleaved):

        byte 0 = left  (two 4-bit nibbles)
        byte 1 = right (two 4-bit nibbles)
        byte 2 = left
        byte 3 = right
        ...

        Output: L, R, L, R, ...
    */

    while (in_pos + 1 < input_size) {

        uint8_t lb = input[in_pos++];   // left byte
        uint8_t rb = input[in_pos++];   // right byte

        // low nibble = first sample
        int16_t l0 = ima_decode_nibble(lb & 0x0F, pred_l, step_l);
        int16_t r0 = ima_decode_nibble(rb & 0x0F, pred_r, step_r);

        // high nibble = second sample
        int16_t l1 = ima_decode_nibble((lb >> 4) & 0x0F, pred_l, step_l);
        int16_t r1 = ima_decode_nibble((rb >> 4) & 0x0F, pred_r, step_r);

        // FIX (Bug 9): was `out_pos + 3 >= max_output` which stops 1 frame early
        // every chunk (wastes the last L/R pair), causing a tiny pitch shift over
        // time because fewer samples are produced per compressed byte than expected.
        // Correct check: we are about to write 4 samples (indices +0..+3), so we
        // need out_pos + 4 <= max_output, i.e. out_pos + 4 > max_output to break.
        if (out_pos + 4 > max_output) {
            break;
        }

        output[out_pos++] = l0;
        output[out_pos++] = r0;
        output[out_pos++] = l1;
        output[out_pos++] = r1;
    }

    *output_size = out_pos;
    return (out_pos > 0);
}

// ===========================================
// MS ADPCM DECODER (Simplified)
// ===========================================

bool decode_ms_adpcm(const uint8_t* input, uint32_t input_size,
                     int16_t* output, uint32_t* output_size) {
    if (!input || !output || !output_size) return false;

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
    uint32_t channels    = player->stream->adi_header.channels;

    if (sample_rate == 0) sample_rate = 48000;
    if (channels == 0)    channels = 2;
    if (channels > 2)     channels = 2;
    if (sample_rate > 48000) sample_rate = 48000;
    if (sample_rate < 8000)  sample_rate = 8000;

    // FIX (Bug 1 - root cause): pcm_capacity must be the number of *samples*
    // (interleaved L+R values) that fit in ONE half of the double buffer.
    //
    // The old formula was:  AUDIO_BUFFER_SIZE * 4 * channels
    //   = 8192 frames * 4 * 2 channels = 65536
    // That was then passed as max_output to decode_ima_adpcm, which interprets
    // it as a sample count.  But AC97_BUFFER_BYTES = 8192 frames * 4 bytes/frame
    // = 32768 bytes = 16384 samples.  So max_output was 4× too large in bytes
    // but when the decoder was actually constrained by input bytes it would only
    // produce ~8192 samples instead of 16384 → half the audio per buffer → half
    // speed.  Simultaneously the back-buffer pointer was offset by 65536 samples
    // (131072 bytes) instead of 16384 samples (32768 bytes), pointing into
    // unrelated heap memory → "deep fried" garbage audio on buffer switch.
    //
    // Correct formula: one half-buffer holds exactly AUDIO_BUFFER_SIZE frames,
    // each frame has `channels` samples → pcm_capacity = AUDIO_BUFFER_SIZE * channels.
    player->pcm_capacity = AUDIO_BUFFER_SIZE * channels;

    serial_write_str("[PLAYER_CREATE_DBG] Header: format=");
    serial_write_dec(player->format);
    serial_write_str(", rate=");
    serial_write_dec(sample_rate);
    serial_write_str(", ch=");
    serial_write_dec(channels);
    serial_write_str(", capacity=");
    serial_write_dec(player->pcm_capacity);
    serial_write_str("\n");

    // Double buffer: two halves of pcm_capacity samples each.
    uint32_t total_samples = player->pcm_capacity * 2;
    uint32_t buffer_bytes  = total_samples * sizeof(int16_t);

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

    player->pcm_position  = 0;
    player->pcm_size      = 0;
    player->current_buffer = 0;
    player->playing        = false;
    player->loop           = false;

    player->adpcm_pred_l = 0;
    player->adpcm_pred_r = 0;
    player->adpcm_step_l = 0;
    player->adpcm_step_r = 0;

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

    g_audio_state.player  = player;
    g_audio_state.playing = false;

    // FIX (Bug 4): set the hardware sample rate BEFORE starting DMA / prefill
    // so the DAC is configured correctly before any audio is queued.
    // Previously this was called after audio_player_fill_buffer and after
    // g_audio_state.playing = true, meaning DMA could start at the wrong rate.
    if (player->stream->adi_header.sample_rate != 0) {
        ac97_set_sample_rate(player->stream->adi_header.sample_rate);
    }

    // Prefill buffer 0 (front)
    serial_write_str("[PLAY] calling prefill fill_buffer...\n");
    uint32_t samples = audio_player_fill_buffer(player, 0);
    serial_write_str("[PLAY] prefill done, samples=");
    serial_write_dec(samples);
    serial_write_str("\n");

    if (samples == 0) {
        serial_write_str("[AUDIO] prefill failed\n");
        g_audio_state.player = NULL;
        return;
    }

    player->current_buffer = 0;
    player->pcm_position   = 0;
    player->pcm_size       = samples;

    serial_write_str("[PLAY] pcm_buffer=");
    serial_write_dec((uint32_t)(uintptr_t)player->pcm_buffer);
    serial_write_str(" pcm_capacity=");
    serial_write_dec(player->pcm_capacity);
    serial_write_str(" pcm_size=");
    serial_write_dec(player->pcm_size);
    serial_write_str("\n");

    serial_write_str("[PLAY] setting playing=true, about to return\n");
    g_audio_state.playing = true;
    player->playing = true;

    // FIX (Bug 5): Start DMA now that the player is ready and sample rate is
    // set.  Previously ac97_init() started DMA immediately at boot, causing a
    // desync between CIV and g_last_valid before any audio existed.
    ac97_start();

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
        // FIX (Bug 2): Decode into the buffer that is NOT currently being read.
        // Old code used `fill_front = (current_buffer == 1)` and then set
        // `current_buffer = fill_front ? 0 : 1` — the boolean inversion was
        // correct but the naming was backwards and confusing, and crucially it
        // called fill_buffer with a `bool` which was cast to 0/1 for the
        // half-buffer index.  With the new fill_buffer signature taking an
        // explicit uint8_t buf_idx this is now unambiguous.
        uint8_t next_buf = player->current_buffer ^ 1;  // always the other half
        uint32_t samples = audio_player_fill_buffer(player, next_buf);
        if (samples == 0) {
            g_audio_state.playing = false;
            player->playing = false;
            g_audio_state.player = NULL;
            return false;
        }

        // Switch to the freshly filled buffer
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

    serial_write_str("[CMD_PLAY] Heap used=");
    serial_write_dec((uint32_t)allocator_used_bytes());
    serial_write_str(" free=");
    serial_write_dec((uint32_t)allocator_free_bytes());
    serial_write_str("\n");

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