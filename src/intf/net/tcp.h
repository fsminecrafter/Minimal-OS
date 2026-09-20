#ifndef TCP_H
#define TCP_H
#include "net/net.h"

typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSE_WAIT,
    // Appended, not inserted: net_syscall.c maps these by name, but
    // keeping the existing values stable costs nothing.
    TCP_SYN_RCVD,       // passive open: SYN seen, SYN|ACK sent, awaiting ACK
} tcp_state_t;

/*
 * Connection table size. Was 4, which was enough for a client that only
 * ever has one connection open. A server holds one connection per
 * active client PLUS whatever is sitting in a listener's backlog, and a
 * .run program on the same box may still want to make its own outbound
 * connection, so it is now 8.
 *
 * Cost: TCP_MAX_CONNS * TCP_RX_BUF_SIZE of kernel .bss = 256 KiB.
 * net_syscall.c sizes its handle table from this, so the two cannot
 * drift apart.
 */
#define TCP_MAX_CONNS 8

/* Listening sockets. One is all Deliver needs; two leaves room. */
#define TCP_MAX_LISTENERS 2

/* Largest accept backlog a listener may ask for (SYN_RCVD + established
 * but not yet accepted). */
#define TCP_MAX_BACKLOG 4

/*
 * Receive buffer, per connection. 8 KiB was too small for bulk
 * transfers: a peer with a real stack (any Linux sender) fills it
 * faster than a single-threaded polled reader drains it, and because
 * this stack has no reassembly queue, everything past the buffer is
 * dropped and retransmitted. 32 KiB with a correctly advertised
 * window (see tcp_send_segment) keeps a file download moving instead
 * of collapsing into retransmit cycles.
 *
 * Cost: see TCP_MAX_CONNS above.
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

    // Passive-open bookkeeping. `accept_pending` is set when the SYN
    // arrives and cleared by tcp_accept(); `accept_order` is a
    // monotonically increasing ticket so accept is FIFO rather than
    // table-order. Both are meaningless on an actively opened
    // connection (they stay zero).
    bool     accept_pending;
    uint32_t accept_order;
} tcp_conn_t;

void tcp_init(void);
void tcp_handle_packet(uint32_t src_ip, const uint8_t* seg, uint16_t len);

tcp_conn_t* tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);

/*
 * Passive open.
 *
 *   tcp_listen(port, backlog)   start accepting SYNs on `port`. Returns
 *                               false if the port is already listening
 *                               or the listener table is full.
 *   tcp_unlisten(port)          stop; connections still in the backlog
 *                               (never accepted) are dropped.
 *   tcp_accept(port, ...)       NON-BLOCKING. Returns the oldest fully
 *                               established connection waiting on `port`,
 *                               or NULL if there is none yet. The
 *                               remote address is written to the
 *                               out-params (host order) if non-NULL.
 *
 * Nothing here polls the NIC: like the rest of the stack it only makes
 * progress when somebody calls network_manager_poll(), so a caller
 * waiting for a connection must poll (net_syscall.c does this in the
 * ACCEPT op).
 */
bool tcp_listen(uint16_t port, uint32_t backlog);
void tcp_unlisten(uint16_t port);
bool tcp_is_listening(uint16_t port);
tcp_conn_t* tcp_accept(uint16_t port, uint32_t* out_remote_ip, uint16_t* out_remote_port);
int32_t tcp_send(tcp_conn_t* conn, const void* data, uint32_t len, uint32_t timeout_ms);
int32_t tcp_recv(tcp_conn_t* conn, void* buf, uint32_t maxlen);
void tcp_poll(void);
bool tcp_is_closed(tcp_conn_t* conn);
void tcp_close(tcp_conn_t* conn);

// Drops the connection immediately, sending a RST if the peer knows about
// it. Never blocks (tcp_close() waits up to 500 ms for the FIN
// handshake), which is what net_syscall.c needs when it reclaims a
// connection whose owning process has died.
void tcp_abort(tcp_conn_t* conn);

#endif
