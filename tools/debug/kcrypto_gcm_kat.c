#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "x86_64/kcrypto.h"

int main(void) {
    static const uint8_t key[16] = {0};
    static const uint8_t iv[12] = {0};
    static const uint8_t plain[16] = {0};
    static const uint8_t expected_cipher[16] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,
        0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78
    };
    static const uint8_t expected_tag[16] = {
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,
        0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf
    };
    static const uint8_t tls_aad[5] = { 0x17,0x03,0x03,0x00,0x35 };
    static const uint8_t tls_plain[37] = { 0 };
    static const uint8_t tls_expected[53] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78,
        0xf7,0x95,0xaa,0xab,0x49,0x4b,0x59,0x23,0xf7,0xfd,0x89,0xff,0x94,0x8b,0xc1,0xe0,
        0x20,0x02,0x11,0x21,0x4e,0x64,0xb8,0x9d,0x55,0x6c,0x65,0x77,0x83,0x97,0x2e,0xbb,
        0xa1,0x6f,0x47,0xcb,0xcb
    };
    uint8_t cipher[16], tag[16], recovered[16];
    uint8_t tls_cipher[37], tls_tag[16], tls_recovered[37];

    kcrypto_aes128_gcm_encrypt_aad(key, iv, NULL, 0, plain, sizeof(plain),
                                   cipher, tag);
    if (memcmp(cipher, expected_cipher, sizeof(cipher)) != 0 ||
        memcmp(tag, expected_tag, sizeof(tag)) != 0 ||
        !kcrypto_aes128_gcm_decrypt_aad(key, iv, NULL, 0, cipher,
                                         sizeof(cipher), tag, recovered) ||
        memcmp(recovered, plain, sizeof(plain)) != 0) {
        fputs("AES-128-GCM KAT failed\n", stderr);
        return 1;
    }
    kcrypto_aes128_gcm_encrypt_aad(key, iv, tls_aad, sizeof(tls_aad), tls_plain,
                                   sizeof(tls_plain), tls_cipher, tls_tag);
    if (memcmp(tls_cipher, tls_expected, sizeof(tls_cipher)) != 0 ||
        memcmp(tls_tag, tls_expected + sizeof(tls_cipher), sizeof(tls_tag)) != 0 ||
        !kcrypto_aes128_gcm_decrypt_aad(key, iv, tls_aad, sizeof(tls_aad),
                                        tls_cipher, sizeof(tls_cipher), tls_tag,
                                        tls_recovered) ||
        memcmp(tls_recovered, tls_plain, sizeof(tls_plain)) != 0) {
        fputs("AES-128-GCM TLS AAD KAT failed\n", stderr);
        return 1;
    }
    uint8_t inplace[37];
    memcpy(inplace, tls_plain, sizeof(inplace));
    kcrypto_aes128_gcm_encrypt_aad(key, iv, tls_aad, sizeof(tls_aad), inplace,
                                   sizeof(inplace), inplace, tls_tag);
    if (memcmp(inplace, tls_expected, sizeof(inplace)) != 0 ||
        memcmp(tls_tag, tls_expected + sizeof(inplace), sizeof(tls_tag)) != 0) {
        fputs("AES-128-GCM in-place KAT failed\n", stderr);
        return 1;
    }
    puts("AES-128-GCM KAT passed");
    return 0;
}
