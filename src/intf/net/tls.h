// src/intf/net/tls.h
#ifndef NET_TLS_H
#define NET_TLS_H

#include <stdint.h>
#include <stdbool.h>
#include "net/tcp.h"

/*
 * ktls - minimal record layer over tcp_conn_t. NOT real TLS.
 *
 * SECURITY: the 32-byte session key is sent IN THE CLEAR in the client
 * hello, so a passive eavesdropper can decrypt everything. Records carry
 * no sequence number and one key serves both directions, so an active
 * attacker can also drop/replay/reflect records. This is a transport
 * shape, not confidentiality. Same trust model as Deliver's KEY: exchange.
 *
 * Wire format:
 *   client -> server: "KTLS1\0" + 32-byte key        (38 bytes)
 *   server -> client: 0x01
 *   records (both ways): u32be len | nonce(12) | ciphertext | tag(16)
 *
 * Two APIs, do not mix them on one connection:
 *   Blocking (kernel code):   ktls_connect / ktls_server_handshake /
 *                             ktls_send / ktls_recv
 *   Incremental (SYS_NET):    ktls_client_begin / ktls_server_begin /
 *                             ktls_handshake_step / ktls_send / ktls_read
 */

typedef enum {
    KTLS_STATE_DEAD = 0,
    KTLS_STATE_HANDSHAKE,
    KTLS_STATE_ESTABLISHED,
} ktls_state_t;

#define KTLS_HELLO_LEN 38   /* 6 magic + 32 key */

typedef struct {
    tcp_conn_t*  conn;
    uint8_t      key[32];
    bool         established;      /* == (state == ESTABLISHED) */
    ktls_state_t state;
    bool         is_server;

    /* incremental handshake */
    uint64_t     hs_deadline_ms;
    uint8_t      hs_buf[KTLS_HELLO_LEN];
    uint32_t     hs_have;

    /* incremental record reassembly (ktls_read) */
    uint8_t      rx_hdr[4];
    uint32_t     rx_hdr_have;
    uint32_t     rx_body_len;      /* 0 = header not complete yet */
    uint32_t     rx_body_have;
    uint8_t*     rx_body;          /* heap */
    uint8_t*     pt;               /* decrypted, not yet consumed (heap) */
    uint32_t     pt_len, pt_pos;
} ktls_conn_t;

/* ---- blocking API (unchanged behaviour) ---- */
ktls_conn_t* ktls_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);
ktls_conn_t* ktls_server_handshake(tcp_conn_t* conn, uint32_t timeout_ms);
int32_t      ktls_recv(ktls_conn_t* k, void* buf, uint32_t cap, uint32_t timeout_ms);

/* ---- incremental API ---- */

// TCP connect + send hello. Returns NULL on failure. Handshake completes
// via ktls_handshake_step(). timeout_ms (0 = 5000) bounds the handshake.
ktls_conn_t* ktls_client_begin(uint32_t ip, uint16_t port, uint32_t timeout_ms);

// Wraps an already-accepted connection. Does NOT take ownership of
// `conn` if it returns NULL. On success ktls_close()/ktls_abort() close it.
ktls_conn_t* ktls_server_begin(tcp_conn_t* conn);

// Never blocks for long. 1 = established, 0 = still waiting, -1 = failed
// (state becomes DEAD; caller should ktls_close()).
int ktls_handshake_step(ktls_conn_t* k);

// Byte-stream read. >0 = bytes, 0 = nothing yet, -1 = dead/peer closed
// and nothing left buffered. Buffered plaintext is always delivered first.
int32_t ktls_read(ktls_conn_t* k, void* buf, uint32_t cap);

/* ---- common ---- */

// One record, <= 64 KiB. All-or-nothing; on failure the stream is
// corrupt and the connection is marked DEAD.
bool ktls_send(ktls_conn_t* k, const void* data, uint32_t len);

// Graceful (FIN, may wait up to 500 ms) close + free.
void ktls_close(ktls_conn_t* k);

// Immediate RST-style teardown + free. Never blocks. Used when reclaiming
// a dead process's connection.
void ktls_abort(ktls_conn_t* k);

#endif // NET_TLS_H
