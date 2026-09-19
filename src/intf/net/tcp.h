#ifndef TCP_H
#define TCP_H
#include "net/net.h"

typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSE_WAIT,
} tcp_state_t;

/*
 * Receive buffer, per connection. 8 KiB was too small for bulk
 * transfers: a peer with a real stack (any Linux sender) fills it
 * faster than a single-threaded polled reader drains it, and because
 * this stack has no reassembly queue, everything past the buffer is
 * dropped and retransmitted. 32 KiB with a correctly advertised
 * window (see tcp_send_segment) keeps a file download moving instead
 * of collapsing into retransmit cycles.
 *
 * Cost: TCP_MAX_CONNS * TCP_RX_BUF_SIZE of kernel .bss = 128 KiB.
 */
#define TCP_RX_BUF_SIZE 32768

typedef struct {
    bool in_use;
    tcp_state_t state;

    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;

    uint32_t snd_nxt;
    uint32_t snd_una;
    uint32_t rcv_nxt;

    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    uint32_t rx_len;

    bool remote_closed;
    bool reset;
    uint64_t last_activity_ms;
} tcp_conn_t;

void tcp_init(void);
void tcp_handle_packet(uint32_t src_ip, const uint8_t* seg, uint16_t len);

tcp_conn_t* tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);
int32_t tcp_send(tcp_conn_t* conn, const void* data, uint32_t len, uint32_t timeout_ms);
int32_t tcp_recv(tcp_conn_t* conn, void* buf, uint32_t maxlen);
void tcp_poll(void);
bool tcp_is_closed(tcp_conn_t* conn);
void tcp_close(tcp_conn_t* conn);

#endif
