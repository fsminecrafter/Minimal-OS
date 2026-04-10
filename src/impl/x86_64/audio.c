#include "audio.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/ac97_driver.h"
#include "graphics.h"

static audio_player_t* g_current_player = NULL;

// ===========================================
// AUDIO HARDWARE BRIDGE
// ===========================================

static int16_t audio_apply_volume(int16_t sample, uint8_t volume) {
    if (volume >= 100) return sample;
    return (int16_t)(((int32_t)sample * volume) / 100);
}

static bool audio_player_fill_buffer(audio_player_t* player) {
    if (!player || !player->stream) return false;

    uint32_t bytes_read = 0;
    uint8_t* compressed = readstream(player->stream, &bytes_read);

    if (!compressed || bytes_read == 0) {
        if (player->loop) {
            seekstream(player->stream, player->stream->adi_header.data_offset);
            bytes_read = 0;
            compressed = readstream(player->stream, &bytes_read);
        }
        if (!compressed || bytes_read == 0) {
            player->playing = false;
            return false;
        }
    }

    uint32_t decoded_samples = 0;
    bool success = false;

    switch (player->format) {
        case AUDIO_FORMAT_IMA_ADPCM:
            success = decode_ima_adpcm(compressed, bytes_read,
                                       player->pcm_buffer, &decoded_samples);
            break;
        case AUDIO_FORMAT_MS_ADPCM:
            success = decode_ms_adpcm(compressed, bytes_read,
                                      player->pcm_buffer, &decoded_samples);
            break;
        case AUDIO_FORMAT_FLAC:
            success = decode_flac(compressed, bytes_read,
                                  player->pcm_buffer, &decoded_samples);
            break;
        case AUDIO_FORMAT_PCM16:
            memcpy(player->pcm_buffer, compressed,
                   bytes_read < player->pcm_buffer_size * 2 ?
                   bytes_read : player->pcm_buffer_size * 2);
            decoded_samples = bytes_read / 2;
            success = true;
            break;
        default:
            success = false;
            break;
    }

    if (!success || decoded_samples == 0) {
        player->playing = false;
        return false;
    }

    player->pcm_position = 0;
    player->pcm_buffer_size = decoded_samples;
    updatestream(player->stream);
    return true;
}

void audio_init(void) {
    ac97_init();
}

void audio_update(void) {
    ac97_update();
}

void audio_mix_streams(int16_t* out, uint32_t frames) {
    if (!out || frames == 0) return;
    uint32_t out_samples = frames * AUDIO_CHANNELS;
    audio_player_t* player = g_current_player;

    if (!player || !player->playing) {
        memset(out, 0, out_samples * sizeof(int16_t));
        return;
    }

    uint8_t src_channels = player->stream ? player->stream->adi_header.channels : AUDIO_CHANNELS;
    if (src_channels == 0) src_channels = AUDIO_CHANNELS;
    uint8_t out_channels = AUDIO_CHANNELS;
    uint8_t volume = player->volume;
    if (volume > 100) volume = 100;

    uint32_t out_idx = 0;
    while (out_idx < out_samples) {
        if (player->pcm_position >= player->pcm_buffer_size) {
            if (!audio_player_fill_buffer(player)) {
                memset(out + out_idx, 0, (out_samples - out_idx) * sizeof(int16_t));
                return;
            }
        }

        if (src_channels <= 1) {
            int16_t s = player->pcm_buffer[player->pcm_position++];
            s = audio_apply_volume(s, volume);
            if (out_channels == 1) {
                out[out_idx++] = s;
            } else {
                out[out_idx++] = s;
                if (out_idx < out_samples) out[out_idx++] = s;
            }
        } else {
            if (player->pcm_position + 1 >= player->pcm_buffer_size) {
                continue;
            }
            int16_t l = player->pcm_buffer[player->pcm_position++];
            int16_t r = player->pcm_buffer[player->pcm_position++];
            if (src_channels > 2) {
                uint32_t skip = src_channels - 2;
                if (player->pcm_position + skip <= player->pcm_buffer_size) {
                    player->pcm_position += skip;
                } else {
                    player->pcm_position = player->pcm_buffer_size;
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

audio_datastream_t* streamfile(const char* path, bool write_mode, 
                               uint32_t chunk_size, bool include_metadata) {
    audio_datastream_t* stream = (audio_datastream_t*)alloc(sizeof(audio_datastream_t));
    if (!stream) return NULL;
    
    memset(stream, 0, sizeof(audio_datastream_t));
    
    strncpy(stream->path, path, sizeof(stream->path) - 1);
    stream->write_mode = write_mode;
    stream->chunk_size = chunk_size;
    stream->include_metadata = include_metadata;
    stream->current_offset = 0;
    stream->end_of_stream = false;
    
    // Allocate chunk buffer
    stream->buffer = (uint8_t*)alloc(chunk_size);
    if (!stream->buffer) {
        free_mem(stream);
        return NULL;
    }
    
    // Open file
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
    }
    
    if (!write_mode && stream->file_handle) {
        stream->total_size = stream->file_handle->data_size;
        
        // Parse ADI header if requested
        if (include_metadata) {
            if (adi_parse_header(path, &stream->adi_header)) {
                stream->is_adi = true;
                stream->current_offset = stream->adi_header.data_offset;
                serial_write_str("Loaded ADI file: ");
                serial_write_str(adi_format_name(stream->adi_header.format));
                serial_write_str("\n");
            }
        }
    }
    
    return stream;
}

uint8_t* readstream(audio_datastream_t* stream, uint32_t* bytes_read) {
    if (!stream || stream->write_mode || stream->end_of_stream) {
        if (bytes_read) *bytes_read = 0;
        return NULL;
    }
    
    if (!stream->file_handle) {
        if (bytes_read) *bytes_read = 0;
        stream->end_of_stream = true;
        return NULL;
    }
    
    // Calculate how much to read
    uint32_t remaining = stream->total_size - stream->current_offset;
    uint32_t to_read = (remaining < stream->chunk_size) ? remaining : stream->chunk_size;
    
    if (to_read == 0) {
        stream->end_of_stream = true;
        if (bytes_read) *bytes_read = 0;
        return NULL;
    }
    
    // Read chunk
    minimafs_seek(stream->file_handle, stream->current_offset);
    uint32_t read = minimafs_read(stream->file_handle, stream->buffer, to_read);
    
    if (bytes_read) *bytes_read = read;
    
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
        offset = stream->total_size;
        stream->end_of_stream = true;
    } else {
        stream->end_of_stream = false;
    }
    
    stream->current_offset = offset;
    return true;
}

uint8_t stream_progress(audio_datastream_t* stream) {
    if (!stream || stream->total_size == 0) return 0;
    
    return (stream->current_offset * 100) / stream->total_size;
}

void closestream(audio_datastream_t* stream) {
    if (!stream) return;
    
    if (stream->file_handle) {
        minimafs_close(stream->file_handle);
    }
    
    if (stream->buffer) {
        free_mem(stream->buffer);
    }
    
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

bool adi_parse_header(const char* path, adi_header_t* header) {
    if (!path || !header) return false;
    
    memset(header, 0, sizeof(adi_header_t));
    
    // Read first 4KB of file for header
    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) return false;
    
    char* buffer = (char*)alloc(4096);
    if (!buffer) {
        minimafs_close(f);
        return false;
    }
    
    uint32_t read = minimafs_read(f, buffer, 4096);
    minimafs_close(f);
    
    if (read == 0) {
        free_mem(buffer);
        return false;
    }
    
    buffer[read - 1] = '\0';  // Ensure null termination
    
    // Parse header fields
    char value[256];
    char* line = buffer;
    uint32_t data_start = 0;
    
    while (line < buffer + read) {
        char* next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        // Parse AudioFormat
        if (strstr(line, "AudioFormat=") == line) {
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                header->format = parse_audio_format(value);
            }
        }
        // Parse AudioLength
        else if (strstr(line, "AudioLength=") == line) {
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                header->length_seconds = parse_int(value);
            }
        }
        // Parse AudioDatalen
        else if (strstr(line, "AudioDatalen=") == line) {
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                header->data_length = parse_int(value);
            }
        }
        // Parse Globalvol
        else if (strstr(line, "Globalvol=") == line) {
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                header->global_volume = parse_int(value);
            }
        }
        // Parse SampleRate
        else if (strstr(line, "SampleRate=") == line) {
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                header->sample_rate = parse_int(value);
            }
        }
        // Parse Channels
        else if (strstr(line, "Channels=") == line) {
            if (getvalfromsplit(line, "=", 2, value, sizeof(value))) {
                header->channels = parse_int(value);
            }
        }
        // Find data section
        else if (strcmp(line, "#DATA") == 0 || strcmp(line, "DATA") == 0) {
            data_start = (next_line ? (next_line - buffer) : read);
            break;
        }
        
        if (!next_line) break;
        line = next_line;
    }
    
    header->data_offset = data_start;
    
    free_mem(buffer);
    
    // Validate
    if (header->format == AUDIO_FORMAT_NONE) {
        serial_write_str("ADI: Invalid format\n");
        return false;
    }
    
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

bool decode_ima_adpcm(const uint8_t* input, uint32_t input_size,
                      int16_t* output, uint32_t* output_size) {
    if (!input || !output || !output_size) return false;
    
    int predictor = 0;
    int step_index = 0;
    uint32_t out_pos = 0;
    
    for (uint32_t i = 0; i < input_size; i++) {
        // Decode two 4-bit samples per byte
        for (int nibble = 0; nibble < 2; nibble++) {
            int code = (nibble == 0) ? (input[i] & 0x0F) : ((input[i] >> 4) & 0x0F);
            
            int step = ima_step_table[step_index];
            int diff = step >> 3;
            
            if (code & 1) diff += step >> 2;
            if (code & 2) diff += step >> 1;
            if (code & 4) diff += step;
            if (code & 8) diff = -diff;
            
            predictor += diff;
            
            // Clamp
            if (predictor > 32767) predictor = 32767;
            if (predictor < -32768) predictor = -32768;
            
            output[out_pos++] = (int16_t)predictor;
            
            // Update step index
            step_index += ima_index_table[code];
            if (step_index < 0) step_index = 0;
            if (step_index > 88) step_index = 88;
        }
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
    if (!path) return NULL;
    
    audio_player_t* player = (audio_player_t*)alloc(sizeof(audio_player_t));
    if (!player) return NULL;
    
    memset(player, 0, sizeof(audio_player_t));
    
    // Create stream
    player->stream = streamfile(path, false, 4096, true);
    if (!player->stream) {
        serial_write_str("audio_player: Failed to open stream\n");
        free_mem(player);
        return NULL;
    }
    
    if (!player->stream->is_adi) {
        serial_write_str("audio_player: Not an ADI file\n");
        closestream(player->stream);
        free_mem(player);
        return NULL;
    }
    
    player->format = player->stream->adi_header.format;
    player->volume = player->stream->adi_header.global_volume;
    
    // Allocate PCM buffer (enough for 1 second of audio)
    uint32_t sample_rate = player->stream->adi_header.sample_rate;
    uint32_t channels = player->stream->adi_header.channels;
    player->pcm_buffer_size = sample_rate * channels;  // 1 second
    
    player->pcm_buffer = (int16_t*)alloc(player->pcm_buffer_size * sizeof(int16_t));
    if (!player->pcm_buffer) {
        closestream(player->stream);
        free_mem(player);
        return NULL;
    }
    
    player->pcm_position = 0;
    player->playing = false;
    player->loop = false;
    
    serial_write_str("Audio player created for: ");
    serial_write_str(path);
    serial_write_str(" (");
    serial_write_str(adi_format_name(player->format));
    serial_write_str(", ");
    serial_write_dec(sample_rate);
    serial_write_str("Hz, ");
    serial_write_dec(channels);
    serial_write_str(" ch)\n");
    
    return player;
}

void audio_player_play(audio_player_t* player) {
    if (!player) return;
    
    player->playing = true;
    player->pcm_position = 0;

    if (player->stream) {
        ac97_set_sample_rate(player->stream->adi_header.sample_rate);
    }
    
    serial_write_str("Audio playback started\n");
}

void audio_player_pause(audio_player_t* player) {
    if (!player) return;
    
    player->playing = false;
    
    serial_write_str("Audio playback paused\n");
}

void audio_player_stop(audio_player_t* player) {
    if (!player) return;
    
    player->playing = false;
    player->pcm_position = 0;
    seekstream(player->stream, player->stream->adi_header.data_offset);
    
    serial_write_str("Audio playback stopped\n");
}

void audio_player_set_volume(audio_player_t* player, uint8_t volume) {
    if (!player) return;
    
    if (volume > 100) volume = 100;
    player->volume = volume;
}

bool audio_player_update(audio_player_t* player) {
    if (!player || !player->playing) return false;
    
    // Check if we need more data
    if (player->pcm_position >= player->pcm_buffer_size) {
        // Decode next chunk
        uint32_t bytes_read = 0;
        uint8_t* compressed = readstream(player->stream, &bytes_read);
        
        if (!compressed || bytes_read == 0) {
            // End of stream
            if (player->loop) {
                // Loop back to start
                seekstream(player->stream, player->stream->adi_header.data_offset);
                player->pcm_position = 0;
                return true;
            } else {
                player->playing = false;
                return false;
            }
        }
        
        // Decode compressed data
        uint32_t decoded_samples = 0;
        bool success = false;
        
        switch (player->format) {
            case AUDIO_FORMAT_IMA_ADPCM:
                success = decode_ima_adpcm(compressed, bytes_read,
                                          player->pcm_buffer, &decoded_samples);
                break;
                
            case AUDIO_FORMAT_MS_ADPCM:
                success = decode_ms_adpcm(compressed, bytes_read,
                                         player->pcm_buffer, &decoded_samples);
                break;
                
            case AUDIO_FORMAT_FLAC:
                success = decode_flac(compressed, bytes_read,
                                     player->pcm_buffer, &decoded_samples);
                break;
                
            case AUDIO_FORMAT_PCM16:
                // Already PCM, just copy
                memcpy(player->pcm_buffer, compressed, 
                       bytes_read < player->pcm_buffer_size * 2 ? 
                       bytes_read : player->pcm_buffer_size * 2);
                decoded_samples = bytes_read / 2;
                success = true;
                break;
                
            default:
                serial_write_str("audio_player: Unknown format\n");
                player->playing = false;
                return false;
        }
        
        if (!success) {
            serial_write_str("audio_player: Decode failed\n");
            player->playing = false;
            return false;
        }
        
        // Reset position
        player->pcm_position = 0;
        player->pcm_buffer_size = decoded_samples;
        
        // Advance stream
        updatestream(player->stream);
    }
    
    // TODO: Feed PCM data to your audio hardware
    // Example integration with your existing audio system:
    /*
    extern void audio_hardware_write(int16_t* samples, uint32_t count, uint8_t volume);
    
    uint32_t to_play = player->pcm_buffer_size - player->pcm_position;
    if (to_play > 512) to_play = 512;  // Play 512 samples at a time
    
    audio_hardware_write(&player->pcm_buffer[player->pcm_position], 
                        to_play, player->volume);
    
    player->pcm_position += to_play;
    */
    
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
    if (argc < 2) {
        graphics_write_textr("Usage: play <path>\n");
        graphics_write_textr("Example: play 0:/music.adi\n");
        return;
    }
    
    // Stop previous player
    if (g_current_player) {
        audio_player_destroy(g_current_player);
        g_current_player = NULL;
    }
    
    const char* path = argv[1];
    
    graphics_write_textr("Loading: ");
    graphics_write_textr(path);
    graphics_write_textr("\n");
    
    // Create player
    g_current_player = audio_player_create(path);
    if (!g_current_player) {
        graphics_write_textr("Failed to load audio file!\n");
        return;
    }
    
    // Start playback
    audio_player_play(g_current_player);
    
    graphics_write_textr("Playing... (press 'p' to pause, 's' to stop)\n");
    
    // Show info
    adi_header_t* hdr = &g_current_player->stream->adi_header;
    
    graphics_write_textr("Format: ");
    graphics_write_textr(adi_format_name(hdr->format));
    graphics_write_textr("\n");
    
    graphics_write_textr("Duration: ");
    char buf[32];
    snprintf(buf, sizeof(buf), "%u seconds\n", hdr->length_seconds);
    graphics_write_textr(buf);
    
    graphics_write_textr("Sample rate: ");
    snprintf(buf, sizeof(buf), "%u Hz\n", hdr->sample_rate);
    graphics_write_textr(buf);
    
    graphics_write_textr("Channels: ");
    snprintf(buf, sizeof(buf), "%u\n", hdr->channels);
    graphics_write_textr(buf);
    
    graphics_write_textr("Volume: ");
    snprintf(buf, sizeof(buf), "%u%%\n", hdr->global_volume);
    graphics_write_textr(buf);
}

void cmd_stop(int argc, const char** argv) {
    if (g_current_player) {
        audio_player_stop(g_current_player);
        graphics_write_textr("Stopped\n");
    } else {
        graphics_write_textr("No audio playing\n");
    }
}

void cmd_pause(int argc, const char** argv) {
    if (g_current_player) {
        if (g_current_player->playing) {
            audio_player_pause(g_current_player);
            graphics_write_textr("Paused\n");
        } else {
            audio_player_play(g_current_player);
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
    
    if (!g_current_player) {
        graphics_write_textr("No audio playing\n");
        return;
    }
    
    int vol = parse_int(argv[1]);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    
    audio_player_set_volume(g_current_player, vol);
    
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
