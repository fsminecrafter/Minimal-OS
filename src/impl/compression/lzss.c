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

/* Compression speed knobs. The decoder does not care about any of these -
 * any stream that follows the layout above decodes - so they can change
 * without touching the format or mkpkg.py.
 *
 * LZSS_MAX_CHAIN bounds how many earlier positions are tried per input
 * byte. The old encoder tried every position in the 4096-byte window
 * (O(size * 4096), minutes for a 256 KiB file in a VM); 128 keeps the
 * ratio within a whisker of it on real package contents (see
 * tests/pkg-host) at a small fraction of the cost. Build with
 * -DLZSS_MAX_CHAIN=1000000 to get the exhaustive search back.
 */
#ifndef LZSS_MAX_CHAIN
#define LZSS_MAX_CHAIN 128
#endif

#define LZSS_HASH_BITS 12
#define LZSS_HASH_SIZE (1u << LZSS_HASH_BITS)
#define LZSS_NONE      0xFFFFFFFFu

static inline uint32_t lzss_hash3(const uint8_t* p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> (32 - LZSS_HASH_BITS);
}

// Records positions [start, start+count) so later bytes can match them.
// A position needs 3 bytes ahead of it to be hashed, so the last two
// bytes of the input are never inserted (and never need to be: no
// match can start there).
static void lzss_insert_range(const uint8_t* data, uint32_t data_size,
                              uint32_t* head, uint32_t* prev,
                              uint32_t start, uint32_t count) {
    for (uint32_t p = start; p < start + count; p++) {
        if (p + 2 >= data_size) break;
        uint32_t h = lzss_hash3(data + p);
        prev[p & (LZSS_N - 1)] = head[h];
        head[h] = p;
    }
}

uint8_t* lzss_compress(const uint8_t* data, uint32_t data_size, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (!data || data_size == 0 || !out_size) return NULL;

    uint32_t capacity = 256;
    uint8_t* out = (uint8_t*)alloc_unzeroed(capacity);
    if (!out) return NULL;
    uint32_t out_pos = 0;

    // Hash-chain match finder state: head[h] is the newest position
    // whose 3 bytes hash to h, prev[p & (N-1)] the previous position
    // with the same hash. A ring of N entries is enough because nothing
    // older than the window is ever followed. 2 * 16 KiB on the heap
    // (never the stack), freed on every exit path below.
    uint32_t* tables = (uint32_t*)alloc_unzeroed(sizeof(uint32_t) * (LZSS_HASH_SIZE + LZSS_N));
    if (!tables) { free_mem(out); return NULL; }
    uint32_t* head = tables;
    uint32_t* prev = tables + LZSS_HASH_SIZE;
    for (uint32_t i = 0; i < LZSS_HASH_SIZE + LZSS_N; i++) tables[i] = LZSS_NONE;

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

            // Walk the candidates that share this position's 3-byte
            // hash, newest first. `>=` (not `>`) keeps the OLDEST of
            // equally long matches, which is what the brute-force
            // encoder this replaced returned, so with an unlimited
            // chain the output is byte-identical to it (tests/pkg-host
            // checks exactly that).
            uint32_t cand = head[lzss_hash3(data + pos)];
            uint32_t chain = LZSS_MAX_CHAIN;
            while (cand != LZSS_NONE && cand >= window_start && chain-- > 0) {
                uint32_t length = 0;
                while (length < max_len && data[cand + length] == data[pos + length]) {
                    length++;
                }
                if (length >= best_len) {
                    best_len = length;
                    best_src = cand;
                }
                uint32_t next = prev[cand & (LZSS_N - 1)];
                if (next != LZSS_NONE && next >= cand) break;   // never loop
                cand = next;
            }
        }

        if (best_len > LZSS_THRESHOLD) {
            uint32_t match_pos    = (LZSS_N - LZSS_F + best_src) & (LZSS_N - 1);
            uint32_t length_field = best_len - (LZSS_THRESHOLD + 1);
            chunk[chunk_len++] = (uint8_t)(match_pos & 0xFF);
            chunk[chunk_len++] = (uint8_t)(((match_pos >> 8) << 4) | (length_field & 0x0F));
            /* flag bit stays 0 for a match */
            lzss_insert_range(data, data_size, head, prev, pos, best_len);
            pos += best_len;
        } else {
            flag_byte |= (uint8_t)(1u << flag_bit);
            chunk[chunk_len++] = data[pos];
            lzss_insert_range(data, data_size, head, prev, pos, 1);
            pos += 1;
        }

        flag_bit++;
        if (flag_bit == 8) {
            uint32_t needed = out_pos + 1 + chunk_len;
            if (needed > capacity) {
                uint32_t new_cap = capacity * 2;
                while (needed > new_cap) new_cap *= 2;
                uint8_t* bigger = (uint8_t*)alloc_resize(out, new_cap);
                if (!bigger) { free_mem(out); free_mem(tables); return NULL; }
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
            if (!bigger) { free_mem(out); free_mem(tables); return NULL; }
            out = bigger;
            capacity = new_cap;
        }
        out[out_pos++] = flag_byte;
        for (uint32_t i = 0; i < chunk_len; i++) out[out_pos++] = chunk[i];
    }

    free_mem(tables);
    *out_size = out_pos;
    return out;
}