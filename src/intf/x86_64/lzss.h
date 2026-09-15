#ifndef LZSS_H
#define LZSS_H

#include <stdint.h>

/*
 * Minimal LZSS decompressor (Okumura-style: 4096-byte window, 3..18
 * byte matches, 1 flag bit per literal/match). Used to unpack .mpkg
 * package archives - see x86_64/pkgformat.h. The matching encoder
 * lives on the host, in tools/pkgbuilder/mkpkg.py - the two MUST stay
 * in exact sync on N/F/THRESHOLD and bit layout, or archives built by
 * one will decode as garbage in the other.
 *
 * Decompresses `in` (in_size bytes) into `out`, which must already be
 * allocated to at least `out_capacity` bytes. Never reads past
 * in_size bytes of `in`, and never writes past out_capacity bytes of
 * `out` - a truncated or corrupt input simply stops early, or, if it
 * would overflow the output buffer, aborts and returns 0.
 *
 * Returns the number of bytes actually written to `out` on success.
 * A legitimate empty input naturally returns 0 too, so callers that
 * need to distinguish "empty" from "corrupt/truncated" should compare
 * the result against the uncompressed size they expected (as
 * installpkg's extractor does).
 */
uint32_t lzss_decompress(const uint8_t* in, uint32_t in_size,
                         uint8_t* out, uint32_t out_capacity);

/*
 * Encodes `data` (data_size bytes) using the same LZSS scheme
 * lzss_decompress() expects (see its comment for the exact format).
 * Returns a heap-allocated buffer (caller must free_mem() it) sized
 * to the actual compressed length, written to *out_size. Returns NULL
 * on OOM or invalid arguments (data_size == 0).
 *
 * This is a brute-force O(input_size * N * F) encoder - fine for
 * small-to-medium files (packages, program bundles) but NOT suitable
 * for large inputs; callers should cap input size and fall back to
 * uncompressed storage above a reasonable threshold. See
 * PKG_ZIP_LZSS_MAX_BYTES in pkgcommand.c for the policy this codebase
 * uses.
 */
uint8_t* lzss_compress(const uint8_t* data, uint32_t data_size, uint32_t* out_size);

#endif // LZSS_H
