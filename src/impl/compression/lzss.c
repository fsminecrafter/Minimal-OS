#include "x86_64/lzss.h"
#include "x86_64/allocator.h"
#include <stddef.h>

/*
 * Okumura-style LZSS codec.
 *
 * MUST stay in exact sync with tools/mkpkg/mkpkg.py's lzss_compress():
 *   - LZSS_N: sliding window size
 *   - LZSS_F: maximum match length
 *   - LZSS_THRESHOLD: minimum match length worth encoding
 *   - Bitstream layout: one flag byte precedes every 8 literal/match
 *     symbols. Flag bit == 1 -> next byte is a literal. Flag bit == 0
 *     -> next TWO bytes are a match: byte0 = low 8 bits of the window
 *     position, byte1's high nibble = high 4 bits of the window
 *     position, byte1's low nibble = (match_len - THRESHOLD - 1).
 */

#define LZSS_N 4096
#define LZSS_F 18
#define LZSS_THRESHOLD 2

/* ============================================================
 * DECOMPRESS
 * ============================================================ */

uint32_t lzss_decompress(const uint8_t* in, uint32_t in_size,
                         uint8_t* out, uint32_t out_capacity) {
    if (!in || !out || out_capacity == 0 || in_size == 0) return 0;

    /* Ring buffer: N bytes of window plus F-1 bytes of slack. Heap-
     * allocated - at N+F-1 (~4.1KB) this would overflow the 4KB
     * kernel stack on its own if it were a local. */
    uint8_t* text_buf = (uint8_t*)alloc_unzeroed(LZSS_N + LZSS_F - 1);
    if (!text_buf) return 0;

    for (uint32_t i = 0; i < LZSS_N - LZSS_F; i++) {
        text_buf[i] = ' ';
    }

    uint32_t r        = LZSS_N - LZSS_F;
    uint32_t in_pos    = 0;
    uint32_t out_pos   = 0;
    uint32_t flags     = 0;

    while (in_pos < in_size) {
        flags >>= 1;
        if ((flags & 0x100) == 0) {
            if (in_pos >= in_size) break;
            flags = in[in_pos++] | 0xFF00;
        }

        if (flags & 1) {
            /* ---- literal ---- */
            if (in_pos >= in_size) break;
            uint8_t c = in[in_pos++];

            if (out_pos >= out_capacity) { free_mem(text_buf); return 0; }
            out[out_pos++] = c;

            text_buf[r++] = c;
            r &= (LZSS_N - 1);
        } else {
            /* ---- match: needs 2 input bytes ---- */
            if (in_pos + 2 > in_size) break;
            uint32_t byte0 = in[in_pos++];
            uint32_t byte1 = in[in_pos++];

            uint32_t match_pos = byte0 | ((byte1 & 0xF0) << 4);
            uint32_t match_len = (byte1 & 0x0F) + LZSS_THRESHOLD; /* loop runs match_len+1 times */

            for (uint32_t k = 0; k <= match_len; k++) {
                uint8_t c = text_buf[(match_pos + k) & (LZSS_N - 1)];

                if (out_pos >= out_capacity) { free_mem(text_buf); return 0; }
                out[out_pos++] = c;

                text_buf[r++] = c;
                r &= (LZSS_N - 1);
            }
        }
    }

    free_mem(text_buf);
    return out_pos;
}

/* ============================================================
 * COMPRESS
 *
 * Operates directly on the in-memory `data` array rather than
 * simulating the decoder's ring buffer - since we have the entire
 * uncompressed input up front, the position of any earlier byte in
 * `data` IS its eventual decompressed output position, so
 * match_pos = (N - F + src) & (N - 1) (same formula the Python
 * encoder in tools/mkpkg/mkpkg.py uses) reproduces exactly what the
 * ring-buffer-based decoder above expects, with no need to track a
 * ring buffer here at all.
 * ============================================================ */

uint8_t* lzss_compress(const uint8_t* data, uint32_t data_size, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (!data || data_size == 0 || !out_size) return NULL;

    uint32_t capacity = 256;
    uint8_t* out = (uint8_t*)alloc_unzeroed(capacity);
    if (!out) return NULL;
    uint32_t out_pos = 0;

    uint8_t  flag_byte = 0;
    uint8_t  flag_bit  = 0;
    uint8_t  chunk[16]; /* worst case: 8 symbols, each a 2-byte match */
    uint32_t chunk_len = 0;

    uint32_t pos = 0;
    while (pos < data_size) {
        uint32_t max_len = (data_size - pos < LZSS_F) ? (data_size - pos) : LZSS_F;
        uint32_t best_len = 0;
        uint32_t best_src = 0;

        if (max_len >= LZSS_THRESHOLD + 1) {
            uint32_t window_start = (pos > LZSS_N) ? (pos - LZSS_N) : 0;
            for (uint32_t src = window_start; src < pos; src++) {
                uint32_t length = 0;
                while (length < max_len && data[src + length] == data[pos + length]) {
                    length++;
                }
                if (length > best_len) {
                    best_len = length;
                    best_src = src;
                    if (length >= max_len) break;
                }
            }
        }

        if (best_len > LZSS_THRESHOLD) {
            uint32_t match_pos    = (LZSS_N - LZSS_F + best_src) & (LZSS_N - 1);
            uint32_t length_field = best_len - (LZSS_THRESHOLD + 1);
            chunk[chunk_len++] = (uint8_t)(match_pos & 0xFF);
            chunk[chunk_len++] = (uint8_t)(((match_pos >> 8) << 4) | (length_field & 0x0F));
            /* flag bit stays 0 for a match */
            pos += best_len;
        } else {
            flag_byte |= (uint8_t)(1u << flag_bit);
            chunk[chunk_len++] = data[pos];
            pos += 1;
        }

        flag_bit++;
        if (flag_bit == 8) {
            uint32_t needed = out_pos + 1 + chunk_len;
            if (needed > capacity) {
                uint32_t new_cap = capacity * 2;
                while (needed > new_cap) new_cap *= 2;
                uint8_t* bigger = (uint8_t*)alloc_resize(out, new_cap);
                if (!bigger) { free_mem(out); return NULL; }
                out = bigger;
                capacity = new_cap;
            }
            out[out_pos++] = flag_byte;
            for (uint32_t i = 0; i < chunk_len; i++) out[out_pos++] = chunk[i];
            flag_byte = 0;
            flag_bit  = 0;
            chunk_len = 0;
        }
    }

    /* Flush a partial trailing group */
    if (flag_bit > 0) {
        uint32_t needed = out_pos + 1 + chunk_len;
        if (needed > capacity) {
            uint32_t new_cap = capacity * 2;
            while (needed > new_cap) new_cap *= 2;
            uint8_t* bigger = (uint8_t*)alloc_resize(out, new_cap);
            if (!bigger) { free_mem(out); return NULL; }
            out = bigger;
            capacity = new_cap;
        }
        out[out_pos++] = flag_byte;
        for (uint32_t i = 0; i < chunk_len; i++) out[out_pos++] = chunk[i];
    }

    *out_size = out_pos;
    return out;
}