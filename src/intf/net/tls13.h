#ifndef NET_TLS13_H
#define NET_TLS13_H

#include <stdint.h>
#include <stdbool.h>
#include "net/tcp.h"

/*
 * Small TLS 1.3 client profile for HTTPS:
 *   TLS_AES_128_GCM_SHA256 + X25519
 *
 * This is intentionally a client-only profile. Certificate chains and
 * CertificateVerify are consumed but not authenticated until the kernel has
 * a trust-store implementation. The Finished messages are always checked.
 */
typedef struct tls13_client tls13_client_t;

tls13_client_t* tls13_connect(uint32_t ip, uint16_t port, const char* host,
                              uint32_t timeout_ms);
const char* tls13_last_error(void);
int32_t tls13_send(tls13_client_t* client, const void* data, uint32_t len);
int32_t tls13_recv(tls13_client_t* client, void* data, uint32_t cap,
                   uint32_t timeout_ms);
void tls13_close(tls13_client_t* client);

#endif