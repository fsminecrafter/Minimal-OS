// src/impl/net/tls.c
#include "net/tls.h"
#include "x86_64/kcrypto.h"
#include "x86_64/random.h"
#include "x86_64/allocator.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "string.h"
#include "serial.h"

#define KTLS_MAGIC      "KTLS1\0"
#define KTLS_MAGIC_LEN  6
#define KTLS_STATUS_OK  0x01

#define KTLS_MAX_RECORD (64u * 1024u)   // plaintext cap per record

// ---------------------------------------------------------------------------
// Blocking helpers over tcp_conn_t (mirrors the pattern used throughout
// this kernel's net stack - see mos_tcp_send_all()/dlr_tcp_read_exact()
// for the userspace equivalents this is modeled on).
// ---------------------------------------------------------------------------

static bool ktls_tcp_send_all(tcp_conn_t* conn, const void* buf, uint32_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t sent = 0;
    uint64_t start = time_get_uptime_ms();

    while (sent < len) {
        int32_t n = tcp_send(conn, p + sent, len - sent, 5000);
        if (n <= 0) {
            if (time_get_uptime_ms() - start > 15000) return false;
            sleep(2);
            continue;
        }
        sent += (uint32_t)n;
    }
    return true;
}

static bool ktls_tcp_recv_exact(tcp_conn_t* conn, void* buf, uint32_t len,
                                uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;
    uint64_t start = time_get_uptime_ms();

    while (got < len) {
        int32_t n = tcp_recv(conn, p + got, len - got);
        if (n < 0) return false;                     // peer closed
        if (n == 0) {
            if (time_get_uptime_ms() - start > timeout_ms) return false;
            sleep(2);
            continue;
        }
        got += (uint32_t)n;
        start = time_get_uptime_ms();                // reset idle timer on progress
    }
    return true;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

ktls_conn_t* ktls_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    tcp_conn_t* conn = tcp_connect(ip, port, timeout_ms);
    if (!conn) return NULL;

    ktls_conn_t* k = (ktls_conn_t*)alloc(sizeof(ktls_conn_t));
    if (!k) { tcp_close(conn); return NULL; }
    k->conn = conn;
    k->established = false;

    random_bytes(k->key, sizeof(k->key));

    uint8_t hello[KTLS_MAGIC_LEN + 32];
    memcpy(hello, KTLS_MAGIC, KTLS_MAGIC_LEN);
    memcpy(hello + KTLS_MAGIC_LEN, k->key, 32);

    if (!ktls_tcp_send_all(conn, hello, sizeof(hello))) {
        free_mem(k);
        tcp_close(conn);
        return NULL;
    }

    uint8_t status = 0;
    if (!ktls_tcp_recv_exact(conn, &status, 1, timeout_ms ? timeout_ms : 5000) ||
        status != KTLS_STATUS_OK) {
        free_mem(k);
        tcp_close(conn);
        return NULL;
    }

    k->established = true;
    return k;
}

ktls_conn_t* ktls_server_handshake(tcp_conn_t* conn, uint32_t timeout_ms) {
    if (!conn) return NULL;
    uint32_t wait_ms = timeout_ms ? timeout_ms : 5000;

    uint8_t hello[KTLS_MAGIC_LEN + 32];
    if (!ktls_tcp_recv_exact(conn, hello, sizeof(hello), wait_ms)) return NULL;
    if (memcmp(hello, KTLS_MAGIC, KTLS_MAGIC_LEN) != 0) {
        serial_write_str("ktls: bad handshake magic from client\n");
        return NULL;
    }

    ktls_conn_t* k = (ktls_conn_t*)alloc(sizeof(ktls_conn_t));
    if (!k) return NULL;
    k->conn = conn;
    memcpy(k->key, hello + KTLS_MAGIC_LEN, 32);
    k->established = false;

    uint8_t status = KTLS_STATUS_OK;
    if (!ktls_tcp_send_all(conn, &status, 1)) {
        free_mem(k);
        return NULL;
    }

    k->established = true;
    return k;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

bool ktls_send(ktls_conn_t* k, const void* data, uint32_t len) {
    if (!k || !k->established || !data) return false;
    if (len == 0 || len > KTLS_MAX_RECORD) return false;

    uint32_t body_len = KCRYPTO_GCM_IV_LEN + len + KCRYPTO_GCM_TAG_LEN;
    uint8_t* frame = (uint8_t*)alloc_unzeroed(4u + body_len);
    if (!frame) return false;

    frame[0] = (uint8_t)(body_len >> 24);
    frame[1] = (uint8_t)(body_len >> 16);
    frame[2] = (uint8_t)(body_len >> 8);
    frame[3] = (uint8_t)(body_len);

    uint8_t* nonce = frame + 4;
    uint8_t* ct    = nonce + KCRYPTO_GCM_IV_LEN;
    uint8_t* tag   = ct + len;

    // A fresh random nonce per record. This is the same design
    // Deliver's own dlr_seal() uses and for the same reason: it never
    // requires tracking a send counter across reconnects, and the
    // birthday bound on 96-bit random nonces is astronomically far
    // from anything one session will ever send.
    random_bytes(nonce, KCRYPTO_GCM_IV_LEN);
    memcpy(ct, data, len);
    kcrypto_aes256_gcm_encrypt(k->key, nonce, ct, len, ct, tag);

    bool ok = ktls_tcp_send_all(k->conn, frame, 4u + body_len);
    free_mem(frame);
    return ok;
}

int32_t ktls_recv(ktls_conn_t* k, void* buf, uint32_t cap, uint32_t timeout_ms) {
    if (!k || !k->established || !buf) return -1;

    uint8_t len_bytes[4];
    if (!ktls_tcp_recv_exact(k->conn, len_bytes, 4, timeout_ms)) return 0;

    uint32_t body_len = ((uint32_t)len_bytes[0] << 24) | ((uint32_t)len_bytes[1] << 16) |
                        ((uint32_t)len_bytes[2] << 8) | (uint32_t)len_bytes[3];

    uint32_t min_body = KCRYPTO_GCM_IV_LEN + KCRYPTO_GCM_TAG_LEN;
    if (body_len < min_body || body_len - min_body > KCRYPTO_GCM_IV_LEN + KTLS_MAX_RECORD) {
        // Refuse before allocating: an attacker-controlled length must
        // never size an allocation. Treat as a dead connection.
        serial_write_str("ktls: record length out of range, dropping connection\n");
        return -1;
    }

    uint8_t* body = (uint8_t*)alloc_unzeroed(body_len);
    if (!body) return -1;

    if (!ktls_tcp_recv_exact(k->conn, body, body_len, timeout_ms)) {
        free_mem(body);
        return 0;
    }

    uint32_t pt_len = body_len - min_body;
    if (pt_len > cap) {
        // Caller's buffer is too small for this record - refuse rather
        // than silently truncating a decrypted (and therefore
        // integrity-checked only up to this point) plaintext.
        serial_write_str("ktls: record larger than caller's buffer\n");
        free_mem(body);
        return -1;
    }

    const uint8_t* nonce = body;
    const uint8_t* ct    = body + KCRYPTO_GCM_IV_LEN;
    const uint8_t* tag   = ct + pt_len;

    int ok = kcrypto_aes256_gcm_decrypt(k->key, nonce, ct, pt_len, tag, (uint8_t*)buf);
    free_mem(body);

    if (!ok) {
        serial_write_str("ktls: GCM tag mismatch, dropping connection\n");
        return -1;
    }
    return (int32_t)pt_len;
}

void ktls_close(ktls_conn_t* k) {
    if (!k) return;
    if (k->conn) tcp_close(k->conn);
    memset(k->key, 0, sizeof(k->key));   // don't leave the session key in freed heap memory
    free_mem(k);
}
