// src/intf/x86_64/kcrypto.h
#ifndef KCRYPTO_H
#define KCRYPTO_H

#include <stdint.h>
#include <stddef.h>

/*
 * Freestanding SHA-256 + AES-256-GCM for kernel use.
 *
 * This is the kernel-side counterpart to programs/dlr/dlr_crypto.c /
 * libraries/minimaSSL - same algorithms, same test vectors would apply,
 * but duplicated here because kernel code cannot link userspace .run
 * programs or minimaSSL (built against the SDK's freestanding libc,
 * not the kernel's). If the two ever drift, this file should be
 * checked against dlr_crypto.c's KAT-tested implementation.
 */

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buffered;
} kcrypto_sha256_ctx;

void kcrypto_sha256_init(kcrypto_sha256_ctx* ctx);
void kcrypto_sha256_update(kcrypto_sha256_ctx* ctx, const void* data, size_t len);
void kcrypto_sha256_final(kcrypto_sha256_ctx* ctx, uint8_t out[32]);
void kcrypto_sha256(const void* data, size_t len, uint8_t out[32]);

// ---------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------

#define KCRYPTO_GCM_IV_LEN  12
#define KCRYPTO_GCM_TAG_LEN 16
#define KCRYPTO_KEY_LEN     32

// Encrypt/decrypt in place is allowed (ct may alias pt).
void kcrypto_aes256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12],
                                const uint8_t* pt, size_t len,
                                uint8_t* ct, uint8_t tag[16]);

// Returns 1 if the tag verifies, 0 otherwise. On failure the output
// buffer's contents are undefined and must not be used.
int kcrypto_aes256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12],
                               const uint8_t* ct, size_t len,
                               const uint8_t tag[16], uint8_t* pt);

// Constant-time compare - use for tags/keys, never memcmp.
int kcrypto_memeq_ct(const void* a, const void* b, size_t n);

#endif // KCRYPTO_H
