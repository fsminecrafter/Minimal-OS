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

#define KTLS_MAX_RECORD (64u * 1024u)               // plaintext cap per record
#define KTLS_MIN_BODY   (KCRYPTO_GCM_IV_LEN + KCRYPTO_GCM_TAG_LEN)
#define KTLS_HANDSHAKE_DEFAULT_MS 5000
#define KTLS_SEND_STALL_MS        6000              // no progress this long => give up

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void ktls_mark_dead(ktls_conn_t* k) {
    k->state = KTLS_STATE_DEAD;
    k->established = false;
}

static void ktls_mark_established(ktls_conn_t* k) {
    k->state = KTLS_STATE_ESTABLISHED;
    k->established = true;
}

static ktls_conn_t* ktls_alloc(tcp_conn_t* conn) {
    ktls_conn_t* k = (ktls_conn_t*)alloc(sizeof(ktls_conn_t));   // zeroed
    if (k) k->conn = conn;
    return k;
}

static void ktls_free_buffers(ktls_conn_t* k) {
    if (k->rx_body) { free_mem(k->rx_body); k->rx_body = NULL; }
    if (k->pt) {
        memset(k->pt, 0, k->pt_len);          // plaintext may be sensitive
        free_mem(k->pt);
        k->pt = NULL;
    }
    k->rx_body_len = k->rx_body_have = 0;
    k->rx_hdr_have = 0;
    k->pt_len = k->pt_pos = 0;
}

static void ktls_release(ktls_conn_t* k) {
    ktls_free_buffers(k);
    memset(k->key, 0, sizeof(k->key));
    free_mem(k);
}

// Small fixed-size send (hello / status). tcp_send() already retries for
// ~3 s internally, so a zero/short result means the peer is gone.
static bool ktls_send_small(tcp_conn_t* c, const void* buf, uint32_t len) {
    return tcp_send(c, buf, len, 5000) == (int32_t)len;
}

// Blocking send used by the blocking API and ktls_send(). A dead
// connection (n < 0) fails at once; a stalled peer fails after
// KTLS_SEND_STALL_MS without progress (was: 15 s wall clock, and spun on
// a dead connection).
static bool ktls_tcp_send_all(tcp_conn_t* conn, const void* buf, uint32_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t sent = 0;
    uint64_t last_progress = time_get_uptime_ms();

    while (sent < len) {
        int32_t n = tcp_send(conn, p + sent, len - sent, 5000);
        if (n < 0) return false;
        if (n == 0) {
            if (time_get_uptime_ms() - last_progress > KTLS_SEND_STALL_MS) return false;
            sleep(2);
            continue;
        }
        sent += (uint32_t)n;
        last_progress = time_get_uptime_ms();
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
        if (n < 0) return false;
        if (n == 0) {
            if (time_get_uptime_ms() - start > timeout_ms) return false;
            sleep(2);
            continue;
        }
        got += (uint32_t)n;
        start = time_get_uptime_ms();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Blocking handshake API
// ---------------------------------------------------------------------------

ktls_conn_t* ktls_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    tcp_conn_t* conn = tcp_connect(ip, port, timeout_ms);
    if (!conn) return NULL;

    ktls_conn_t* k = ktls_alloc(conn);
    if (!k) { tcp_close(conn); return NULL; }

    random_bytes(k->key, sizeof(k->key));

    uint8_t hello[KTLS_HELLO_LEN];
    memcpy(hello, KTLS_MAGIC, KTLS_MAGIC_LEN);
    memcpy(hello + KTLS_MAGIC_LEN, k->key, 32);

    if (!ktls_tcp_send_all(conn, hello, sizeof(hello))) {
        ktls_release(k);
        tcp_close(conn);
        return NULL;
    }

    uint8_t status = 0;
    if (!ktls_tcp_recv_exact(conn, &status, 1, timeout_ms ? timeout_ms : 5000) ||
        status != KTLS_STATUS_OK) {
        ktls_release(k);
        tcp_close(conn);
        return NULL;
    }

    ktls_mark_established(k);
    return k;
}

ktls_conn_t* ktls_server_handshake(tcp_conn_t* conn, uint32_t timeout_ms) {
    if (!conn) return NULL;
    uint32_t wait_ms = timeout_ms ? timeout_ms : 5000;

    uint8_t hello[KTLS_HELLO_LEN];
    if (!ktls_tcp_recv_exact(conn, hello, sizeof(hello), wait_ms)) return NULL;
    if (memcmp(hello, KTLS_MAGIC, KTLS_MAGIC_LEN) != 0) {
        serial_write_str("ktls: bad handshake magic from client\n");
        return NULL;
    }

    ktls_conn_t* k = ktls_alloc(conn);
    if (!k) return NULL;
    k->is_server = true;
    memcpy(k->key, hello + KTLS_MAGIC_LEN, 32);

    uint8_t status = KTLS_STATUS_OK;
    if (!ktls_send_small(conn, &status, 1)) {
        ktls_release(k);
        return NULL;
    }

    ktls_mark_established(k);
    return k;
}

// ---------------------------------------------------------------------------
// Incremental handshake
// ---------------------------------------------------------------------------

ktls_conn_t* ktls_client_begin(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = KTLS_HANDSHAKE_DEFAULT_MS;

    tcp_conn_t* conn = tcp_connect(ip, port, timeout_ms);
    if (!conn) return NULL;

    ktls_conn_t* k = ktls_alloc(conn);
    if (!k) { tcp_close(conn); return NULL; }

    random_bytes(k->key, sizeof(k->key));

    uint8_t hello[KTLS_HELLO_LEN];
    memcpy(hello, KTLS_MAGIC, KTLS_MAGIC_LEN);
    memcpy(hello + KTLS_MAGIC_LEN, k->key, 32);

    if (!ktls_send_small(conn, hello, sizeof(hello))) {
        ktls_release(k);
        tcp_close(conn);
        return NULL;
    }

    k->is_server = false;
    k->state = KTLS_STATE_HANDSHAKE;
    k->hs_deadline_ms = time_get_uptime_ms() + timeout_ms;
    return k;
}

ktls_conn_t* ktls_server_begin(tcp_conn_t* conn) {
    if (!conn) return NULL;
    ktls_conn_t* k = ktls_alloc(conn);
    if (!k) return NULL;

    k->is_server = true;
    k->state = KTLS_STATE_HANDSHAKE;
    k->hs_deadline_ms = time_get_uptime_ms() + KTLS_HANDSHAKE_DEFAULT_MS;
    return k;
}

static int ktls_hs_pending(ktls_conn_t* k) {
    if (time_get_uptime_ms() > k->hs_deadline_ms) {
        serial_write_str("ktls: handshake timed out\n");
        ktls_mark_dead(k);
        return -1;
    }
    return 0;
}

int ktls_handshake_step(ktls_conn_t* k) {
    if (!k) return -1;
    if (k->state == KTLS_STATE_ESTABLISHED) return 1;
    if (k->state != KTLS_STATE_HANDSHAKE) return -1;

    if (k->is_server) {
        if (k->hs_have < KTLS_HELLO_LEN) {
            // Only ever ask for what the hello still needs, so any
            // record bytes the client pipelined stay in tcp's buffer.
            int32_t n = tcp_recv(k->conn, k->hs_buf + k->hs_have,
                                 KTLS_HELLO_LEN - k->hs_have);
            if (n < 0) { ktls_mark_dead(k); return -1; }
            k->hs_have += (uint32_t)n;
        }
        if (k->hs_have < KTLS_HELLO_LEN) return ktls_hs_pending(k);

        if (memcmp(k->hs_buf, KTLS_MAGIC, KTLS_MAGIC_LEN) != 0) {
            serial_write_str("ktls: bad handshake magic from client\n");
            ktls_mark_dead(k);
            return -1;
        }
        memcpy(k->key, k->hs_buf + KTLS_MAGIC_LEN, 32);
        memset(k->hs_buf, 0, sizeof(k->hs_buf));

        uint8_t status = KTLS_STATUS_OK;
        if (!ktls_send_small(k->conn, &status, 1)) { ktls_mark_dead(k); return -1; }

        ktls_mark_established(k);
        return 1;
    }

    // client: wait for the one status byte
    uint8_t status = 0;
    int32_t n = tcp_recv(k->conn, &status, 1);
    if (n < 0) { ktls_mark_dead(k); return -1; }
    if (n == 0) return ktls_hs_pending(k);
    if (status != KTLS_STATUS_OK) { ktls_mark_dead(k); return -1; }

    ktls_mark_established(k);
    return 1;
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

    // Fresh random 96-bit nonce per record (same design as dlr_seal()).
    random_bytes(nonce, KCRYPTO_GCM_IV_LEN);
    memcpy(ct, data, len);
    kcrypto_aes256_gcm_encrypt(k->key, nonce, ct, len, ct, tag);

    bool ok = ktls_tcp_send_all(k->conn, frame, 4u + body_len);
    free_mem(frame);

    // A half-written record leaves the peer mid-frame forever.
    if (!ok) ktls_mark_dead(k);
    return ok;
}

// Blocking receive (kernel callers only). Length bound fixed: plaintext,
// not plaintext + IV, is what KTLS_MAX_RECORD limits.
int32_t ktls_recv(ktls_conn_t* k, void* buf, uint32_t cap, uint32_t timeout_ms) {
    if (!k || !k->established || !buf) return -1;

    uint8_t len_bytes[4];
    if (!ktls_tcp_recv_exact(k->conn, len_bytes, 4, timeout_ms)) return 0;

    uint32_t body_len = ((uint32_t)len_bytes[0] << 24) | ((uint32_t)len_bytes[1] << 16) |
                        ((uint32_t)len_bytes[2] << 8) | (uint32_t)len_bytes[3];

    if (body_len < KTLS_MIN_BODY || body_len - KTLS_MIN_BODY > KTLS_MAX_RECORD) {
        serial_write_str("ktls: record length out of range, dropping connection\n");
        return -1;
    }

    uint8_t* body = (uint8_t*)alloc_unzeroed(body_len);
    if (!body) return -1;

    if (!ktls_tcp_recv_exact(k->conn, body, body_len, timeout_ms)) {
        free_mem(body);
        return 0;
    }

    uint32_t pt_len = body_len - KTLS_MIN_BODY;
    if (pt_len > cap) {
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

// Incremental, stream-style read. Never sleeps.
int32_t ktls_read(ktls_conn_t* k, void* buf, uint32_t cap) {
    if (!k || !buf || cap == 0) return -1;

    for (;;) {
        // 1. Hand out already-authenticated plaintext first.
        if (k->pt) {
            uint32_t avail = k->pt_len - k->pt_pos;
            uint32_t n = avail < cap ? avail : cap;
            memcpy(buf, k->pt + k->pt_pos, n);
            k->pt_pos += n;
            if (k->pt_pos >= k->pt_len) {
                memset(k->pt, 0, k->pt_len);
                free_mem(k->pt);
                k->pt = NULL;
                k->pt_len = k->pt_pos = 0;
            }
            return (int32_t)n;
        }
        if (k->state != KTLS_STATE_ESTABLISHED) return -1;

        // 2. Length header (may arrive in pieces).
        if (k->rx_body_len == 0) {
            int32_t n = tcp_recv(k->conn, k->rx_hdr + k->rx_hdr_have,
                                 4 - k->rx_hdr_have);
            if (n < 0) { ktls_mark_dead(k); return -1; }
            if (n == 0) return 0;
            k->rx_hdr_have += (uint32_t)n;
            if (k->rx_hdr_have < 4) return 0;

            uint32_t body_len = ((uint32_t)k->rx_hdr[0] << 24) |
                                ((uint32_t)k->rx_hdr[1] << 16) |
                                ((uint32_t)k->rx_hdr[2] << 8) |
                                 (uint32_t)k->rx_hdr[3];
            k->rx_hdr_have = 0;

            // Attacker-controlled: validate BEFORE it sizes an allocation.
            if (body_len < KTLS_MIN_BODY || body_len - KTLS_MIN_BODY > KTLS_MAX_RECORD) {
                serial_write_str("ktls: record length out of range, dropping connection\n");
                ktls_mark_dead(k);
                return -1;
            }
            k->rx_body = (uint8_t*)alloc_unzeroed(body_len);
            if (!k->rx_body) { ktls_mark_dead(k); return -1; }
            k->rx_body_len = body_len;
            k->rx_body_have = 0;
        }

        // 3. Body (may arrive in pieces).
        int32_t n = tcp_recv(k->conn, k->rx_body + k->rx_body_have,
                             k->rx_body_len - k->rx_body_have);
        if (n < 0) { ktls_mark_dead(k); return -1; }
        if (n == 0) return 0;
        k->rx_body_have += (uint32_t)n;
        if (k->rx_body_have < k->rx_body_len) return 0;

        // 4. Complete record: authenticate + decrypt.
        uint32_t pt_len = k->rx_body_len - KTLS_MIN_BODY;
        const uint8_t* nonce = k->rx_body;
        const uint8_t* ct    = k->rx_body + KCRYPTO_GCM_IV_LEN;
        const uint8_t* tag   = ct + pt_len;

        uint8_t* pt = NULL;
        int ok = 1;
        if (pt_len > 0) {
            pt = (uint8_t*)alloc_unzeroed(pt_len);
            if (!pt) ok = 0;
            else ok = kcrypto_aes256_gcm_decrypt(k->key, nonce, ct, pt_len, tag, pt);
        } else {
            uint8_t dummy;
            ok = kcrypto_aes256_gcm_decrypt(k->key, nonce, ct, 0, tag, &dummy);
        }

        free_mem(k->rx_body);
        k->rx_body = NULL;
        k->rx_body_len = k->rx_body_have = 0;

        if (!ok) {
            serial_write_str("ktls: GCM tag mismatch or OOM, dropping connection\n");
            if (pt) { memset(pt, 0, pt_len); free_mem(pt); }
            ktls_mark_dead(k);
            return -1;
        }
        if (pt_len == 0) continue;          // authenticated empty record: skip

        k->pt = pt;
        k->pt_len = pt_len;
        k->pt_pos = 0;
        // loop: deliver from step 1
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void ktls_close(ktls_conn_t* k) {
    if (!k) return;
    if (k->conn) tcp_close(k->conn);
    ktls_release(k);
}

void ktls_abort(ktls_conn_t* k) {
    if (!k) return;
    if (k->conn) tcp_abort(k->conn);
    ktls_release(k);
}
