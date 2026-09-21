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
    uint8_t cipher[16], tag[16], recovered[16];

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
    puts("AES-128-GCM KAT passed");
    return 0;
}
