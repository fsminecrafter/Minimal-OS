// src/impl/kernel/kcrypto.c
#include "x86_64/kcrypto.h"
#include "string.h"

/* ==========================================================================
 * SHA-256 (FIPS 180-4)
 * ========================================================================== */

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(kcrypto_sha256_ctx* ctx, const uint8_t* block) {
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void kcrypto_sha256_init(kcrypto_sha256_ctx* ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buffered = 0;
}

void kcrypto_sha256_update(kcrypto_sha256_ctx* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    ctx->bitlen += (uint64_t)len * 8u;

    if (ctx->buffered) {
        size_t need = 64 - ctx->buffered;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buffer + ctx->buffered, p, take);
        ctx->buffered += take;
        p += take;
        len -= take;
        if (ctx->buffered < 64) return;
        sha256_block(ctx, ctx->buffer);
        ctx->buffered = 0;
    }

    while (len >= 64) {
        sha256_block(ctx, p);
        p += 64;
        len -= 64;
    }

    if (len) {
        memcpy(ctx->buffer, p, len);
        ctx->buffered = len;
    }
}

void kcrypto_sha256_final(kcrypto_sha256_ctx* ctx, uint8_t out[32]) {
    uint64_t bitlen = ctx->bitlen;
    uint8_t pad = 0x80;

    kcrypto_sha256_ctx tmp = *ctx;
    tmp.bitlen = bitlen;

    size_t buffered = tmp.buffered;
    tmp.buffer[buffered++] = pad;
    if (buffered > 56) {
        while (buffered < 64) tmp.buffer[buffered++] = 0;
        sha256_block(&tmp, tmp.buffer);
        buffered = 0;
    }
    while (buffered < 56) tmp.buffer[buffered++] = 0;

    for (int i = 7; i >= 0; i--) tmp.buffer[buffered++] = (uint8_t)(bitlen >> (i * 8));
    sha256_block(&tmp, tmp.buffer);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(tmp.state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(tmp.state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(tmp.state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(tmp.state[i]);
    }
}

void kcrypto_sha256(const void* data, size_t len, uint8_t out[32]) {
    kcrypto_sha256_ctx ctx;
    kcrypto_sha256_init(&ctx);
    kcrypto_sha256_update(&ctx, data, len);
    kcrypto_sha256_final(&ctx, out);
}

/* ==========================================================================
 * AES-256 (FIPS 197), forward cipher only - GCM never needs the inverse.
 * ========================================================================== */

static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

#define AES256_ROUNDS 14
#define AES256_RK_WORDS (4 * (AES256_ROUNDS + 1))

typedef struct {
    uint32_t rk[AES256_RK_WORDS];
} aes256_key_t;

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t)SBOX[(w >> 24) & 0xFF] << 24) |
           ((uint32_t)SBOX[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)SBOX[(w >> 8) & 0xFF] << 8) |
           ((uint32_t)SBOX[w & 0xFF]);
}

static uint32_t rot_word(uint32_t w) { return (w << 8) | (w >> 24); }

static void aes256_expand_key(const uint8_t key[32], aes256_key_t* out) {
    static const uint8_t RCON[7] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40 };

    for (int i = 0; i < 8; i++) {
        out->rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
                     ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    }
    for (int i = 8; i < AES256_RK_WORDS; i++) {
        uint32_t temp = out->rk[i - 1];
        if (i % 8 == 0) {
            temp = sub_word(rot_word(temp)) ^ ((uint32_t)RCON[i / 8 - 1] << 24);
        } else if (i % 8 == 4) {
            temp = sub_word(temp);
        }
        out->rk[i] = out->rk[i - 8] ^ temp;
    }
}

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

static void add_round_key(uint8_t state[16], const uint32_t* rk) {
    for (int c = 0; c < 4; c++) {
        state[c * 4]     ^= (uint8_t)(rk[c] >> 24);
        state[c * 4 + 1] ^= (uint8_t)(rk[c] >> 16);
        state[c * 4 + 2] ^= (uint8_t)(rk[c] >> 8);
        state[c * 4 + 3] ^= (uint8_t)(rk[c]);
    }
}

static void aes256_encrypt_block(const aes256_key_t* key, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, &key->rk[0]);

    for (int round = 1; round <= AES256_ROUNDS; round++) {
        for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];

        uint8_t t;
        t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13];  s[13] = t;
        t = s[2];  s[2] = s[10]; s[10] = t;
        t = s[6];  s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3];  s[3] = t;

        if (round != AES256_ROUNDS) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = &s[c * 4];
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        add_round_key(s, &key->rk[round * 4]);
    }
    memcpy(out, s, 16);
}

/* ==========================================================================
 * GHASH / GCM (4-bit table method)
 * ========================================================================== */

typedef struct {
    uint8_t table[16][16];
} ghash_key_t;

static void block_rshift1(uint8_t b[16]) {
    for (int i = 15; i > 0; i--) b[i] = (uint8_t)((b[i] >> 1) | ((b[i - 1] & 1) << 7));
    b[0] >>= 1;
}

static void ghash_init(ghash_key_t* gk, const uint8_t h[16]) {
    memset(gk->table[0], 0, 16);
    memcpy(gk->table[8], h, 16);

    for (int i = 4; i > 0; i >>= 1) {
        uint8_t tmp[16];
        memcpy(tmp, gk->table[i * 2], 16);
        int lsb = tmp[15] & 1;
        block_rshift1(tmp);
        if (lsb) tmp[0] ^= 0xE1;
        memcpy(gk->table[i], tmp, 16);
    }

    for (int i = 2; i < 16; i <<= 1) {
        for (int j = 1; j < i; j++) {
            for (int k = 0; k < 16; k++) {
                gk->table[i + j][k] = (uint8_t)(gk->table[i][k] ^ gk->table[j][k]);
            }
        }
    }
}

static void ghash_mul(const ghash_key_t* gk, uint8_t x[16]) {
    static const uint16_t RED[16] = {
        0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
        0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0
    };

    uint8_t z[16];
    memset(z, 0, 16);

    for (int i = 15; i >= 0; i--) {
        uint8_t byte = x[i];
        for (int half = 0; half < 2; half++) {
            uint8_t nibble = half ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
            const uint8_t* row = gk->table[nibble];

            if (!(i == 15 && half == 0)) {
                uint8_t out_nibble = (uint8_t)(z[15] & 0x0F);
                for (int j = 15; j > 0; j--) {
                    z[j] = (uint8_t)((z[j] >> 4) | ((z[j - 1] & 0x0F) << 4));
                }
                z[0] >>= 4;
                z[0] ^= (uint8_t)(RED[out_nibble] >> 8);
                z[1] ^= (uint8_t)(RED[out_nibble] & 0xFF);
            }
            for (int j = 0; j < 16; j++) z[j] ^= row[j];
        }
    }
    memcpy(x, z, 16);
}

static void ghash_update(const ghash_key_t* gk, uint8_t y[16], const uint8_t* data, size_t len) {
    while (len >= 16) {
        for (int i = 0; i < 16; i++) y[i] ^= data[i];
        ghash_mul(gk, y);
        data += 16;
        len -= 16;
    }
    if (len) {
        for (size_t i = 0; i < len; i++) y[i] ^= data[i];
        ghash_mul(gk, y);
    }
}

static void gcm_inc32(uint8_t counter[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++counter[i]) break;
    }
}

static void gcm_core(const aes256_key_t* aes, const uint8_t iv[12],
                     const uint8_t* in, size_t len, uint8_t* out,
                     const uint8_t* ghash_src, uint8_t tag[16]) {
    ghash_key_t gk;
    uint8_t h[16], zero[16];
    memset(zero, 0, 16);
    aes256_encrypt_block(aes, zero, h);
    ghash_init(&gk, h);

    uint8_t j0[16];
    memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    uint8_t counter[16];
    memcpy(counter, j0, 16);

    uint8_t keystream[16];
    size_t offset = 0;
    while (offset < len) {
        gcm_inc32(counter);
        aes256_encrypt_block(aes, counter, keystream);
        size_t n = len - offset;
        if (n > 16) n = 16;
        for (size_t i = 0; i < n; i++) out[offset + i] = (uint8_t)(in[offset + i] ^ keystream[i]);
        offset += n;
    }

    uint8_t y[16];
    memset(y, 0, 16);
    ghash_update(&gk, y, ghash_src, len);

    uint8_t lengths[16];
    memset(lengths, 0, 16);
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) lengths[15 - i] = (uint8_t)(bits >> (i * 8));
    ghash_update(&gk, y, lengths, 16);

    uint8_t s[16];
    aes256_encrypt_block(aes, j0, s);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(y[i] ^ s[i]);
}

void kcrypto_aes256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12],
                                const uint8_t* pt, size_t len,
                                uint8_t* ct, uint8_t tag[16]) {
    aes256_key_t aes;
    aes256_expand_key(key, &aes);
    gcm_core(&aes, iv, pt, len, ct, ct, tag);
}

int kcrypto_aes256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12],
                               const uint8_t* ct, size_t len,
                               const uint8_t tag[16], uint8_t* pt) {
    aes256_key_t aes;
    aes256_expand_key(key, &aes);

    uint8_t computed[16];
    gcm_core(&aes, iv, ct, len, pt, ct, computed);

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(computed[i] ^ tag[i]);
    return diff == 0;
}

/* TLS 1.3 record cipher: AES-128-GCM with arbitrary additional data. */
typedef struct { uint32_t rk[44]; } aes128_key_t;

static void aes128_expand_key(const uint8_t key[16], aes128_key_t* out) {
    static const uint8_t rcon[10] = { 1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36 };
    for (int i = 0; i < 4; i++) {
        out->rk[i] = ((uint32_t)key[4*i] << 24) | ((uint32_t)key[4*i+1] << 16) |
                     ((uint32_t)key[4*i+2] << 8) | key[4*i+3];
    }
    for (int i = 4; i < 44; i++) {
        uint32_t t = out->rk[i - 1];
        if ((i & 3) == 0) t = sub_word(rot_word(t)) ^ ((uint32_t)rcon[i/4-1] << 24);
        out->rk[i] = out->rk[i - 4] ^ t;
    }
}

static void aes128_encrypt_block(const aes128_key_t* key, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, key->rk);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
        uint8_t t;
        t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
        t=s[2]; s[2]=s[10]; s[10]=t;
        t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = &s[c*4];
                uint8_t a0=col[0], a1=col[1], a2=col[2], a3=col[3];
                uint8_t all=(uint8_t)(a0^a1^a2^a3);
                col[0]=(uint8_t)(a0^all^xtime((uint8_t)(a0^a1)));
                col[1]=(uint8_t)(a1^all^xtime((uint8_t)(a1^a2)));
                col[2]=(uint8_t)(a2^all^xtime((uint8_t)(a2^a3)));
                col[3]=(uint8_t)(a3^all^xtime((uint8_t)(a3^a0)));
            }
        }
        add_round_key(s, &key->rk[round*4]);
    }
    memcpy(out, s, 16);
}

static void gcm_core128(const aes128_key_t* aes, const uint8_t iv[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t len, uint8_t* out,
                        const uint8_t* ghash_src,
                        uint8_t tag[16]) {
    ghash_key_t gk;
    uint8_t zero[16] = {0}, h[16];
    aes128_encrypt_block(aes, zero, h);
    ghash_init(&gk, h);
    uint8_t j0[16] = {0};
    memcpy(j0, iv, 12); j0[15] = 1;
    uint8_t counter[16]; memcpy(counter, j0, 16);
    for (size_t off = 0; off < len;) {
        gcm_inc32(counter);
        uint8_t stream[16]; aes128_encrypt_block(aes, counter, stream);
        size_t n = len - off; if (n > 16) n = 16;
        for (size_t i = 0; i < n; i++) out[off+i] = (uint8_t)(in[off+i]^stream[i]);
        off += n;
    }
    uint8_t y[16] = {0};
    ghash_update(&gk, y, aad, aad_len);
    /* GCM authenticates ciphertext on both encryption and decryption. */
    ghash_update(&gk, y, ghash_src, len);
    uint8_t lengths[16] = {0};
    uint64_t ab = (uint64_t)aad_len * 8u, cb = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) { lengths[7-i]=(uint8_t)(ab>>(i*8)); lengths[15-i]=(uint8_t)(cb>>(i*8)); }
    ghash_update(&gk, y, lengths, 16);
    uint8_t s[16]; aes128_encrypt_block(aes, j0, s);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(y[i]^s[i]);
}

void kcrypto_aes128_gcm_encrypt_aad(const uint8_t key[16], const uint8_t iv[12],
                                    const uint8_t* aad, size_t aad_len,
                                    const uint8_t* pt, size_t len,
                                    uint8_t* ct, uint8_t tag[16]) {
    aes128_key_t aes; aes128_expand_key(key, &aes);
    gcm_core128(&aes, iv, aad, aad_len, pt, len, ct, ct, tag);
}

int kcrypto_aes128_gcm_decrypt_aad(const uint8_t key[16], const uint8_t iv[12],
                                   const uint8_t* aad, size_t aad_len,
                                   const uint8_t* ct, size_t len,
                                   const uint8_t tag[16], uint8_t* pt) {
    aes128_key_t aes; aes128_expand_key(key, &aes);
    uint8_t computed[16]; gcm_core128(&aes, iv, aad, aad_len, ct, len, pt, ct, computed);
    uint8_t diff = 0; for (int i = 0; i < 16; i++) diff |= (uint8_t)(computed[i]^tag[i]);
    return diff == 0;
}

int kcrypto_memeq_ct(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}
