#include "net/tcp.h"
#include "net/ip.h"
#include "x86_64/network_manager.h"
#include "x86_64/scheduler.h"
#include "x86_64/random.h"
#include "time.h"
#include "string.h"
#include "serial.h"

/* Retained for reference; the advertised window is now computed from
 * the free space in conn->rx_buf on every segment. */
#define TCP_DEFAULT_WINDOW 4096
#define TCP_RETRANSMIT_MS 500
#define TCP_MAX_RETRIES 6

/*
 * Send-side pacing. tcp_send() used to be stop-and-wait: one 1 KiB
 * segment, then block until it was ACKed. That is fine against a Linux
 * peer (a full-sized segment is ACKed at once) but it is a cliff
 * against anything that delays ACKs - a Windows client waits for a
 * second segment or a 200 ms timer, which caps stop-and-wait at about
 * five segments a second. A server pushing a package to such a client
 * would crawl. Keeping a few segments in flight lets the peer's "ACK
 * every second segment" rule do its job. There is still no congestion
 * control and the peer's advertised window is still not read; on a LAN
 * with 4 KiB in flight that is a deliberate simplification, not an
 * oversight.
 */
#define TCP_SEG_SIZE          1024
#define TCP_SEND_WINDOW_SEGS  4

/* A half-open passive connection that never completes the handshake
 * must not hold a table slot forever. */
#define TCP_SYN_RCVD_TIMEOUT_MS 5000

static tcp_conn_t g_conns[TCP_MAX_CONNS];
static uint16_t g_next_ephemeral_port = 49152;

typedef struct {
    bool     used;
    uint16_t port;
    uint32_t backlog;
} tcp_listener_t;

static tcp_listener_t g_listeners[TCP_MAX_LISTENERS];
static uint32_t g_accept_seq = 0;

void tcp_init(void) {
    memset(g_conns, 0, sizeof(g_conns));
    memset(g_listeners, 0, sizeof(g_listeners));
    g_accept_seq = 0;
}

void tcp_poll(void) {
    network_manager_poll();
}

static tcp_conn_t* tcp_alloc(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) if (!g_conns[i].in_use) return &g_conns[i];
    return NULL;
}

static tcp_conn_t* tcp_find(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (c->in_use && c->remote_ip == remote_ip &&
            c->remote_port == remote_port && c->local_port == local_port) return c;
    }
    return NULL;
}

/*
 * Stateless segment transmit. Split out of tcp_send_segment() so that a
 * RST can be sent for a segment that has no connection behind it - a
 * tcp_conn_t is 32 KiB and the kernel stack is 4 KiB, so "build a
 * temporary connection on the stack" is not available.
 */
static bool tcp_send_raw(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                         uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                         const void* data, uint16_t data_len) {
    static uint8_t buf[ETH_MTU];
    tcp_header_t* hdr = (tcp_header_t*)buf;
    hdr->src_port = net_htons(local_port);
    hdr->dst_port = net_htons(remote_port);
    hdr->seq = net_htonl(seq);
    hdr->ack = net_htonl((flags & TCP_FLAG_ACK) ? ack : 0);
    hdr->data_offset = (uint8_t)(5 << 4);
    hdr->flags = flags;
    hdr->window = net_htons(window);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data_len > sizeof(buf) - sizeof(tcp_header_t)) return false;
    if (data && data_len) memcpy(buf + sizeof(tcp_header_t), data, data_len);

    uint16_t seg_len = (uint16_t)(sizeof(tcp_header_t) + data_len);
    hdr->checksum = net_htons(net_tcp_checksum(ip_get_local(), remote_ip, buf, seg_len));

    return ip_send(remote_ip, IP_PROTO_TCP, buf, seg_len);
}

static bool tcp_send_segment(tcp_conn_t* conn, uint8_t flags, const void* data, uint16_t data_len) {
    /*
     * Advertise the space actually left in the receive buffer, not a
     * fixed constant. The old fixed 4096 lied in both directions: it
     * under-advertised on an empty buffer (throttling the sender for
     * no reason) and over-advertised on a nearly full one (inviting
     * data this stack has nowhere to put, which it then silently
     * dropped because there is no reassembly queue).
     */
    uint32_t rx_space = (conn->rx_len < TCP_RX_BUF_SIZE)
                        ? (TCP_RX_BUF_SIZE - conn->rx_len) : 0;
    if (rx_space > 0xFFFFu) rx_space = 0xFFFFu;

    return tcp_send_raw(conn->remote_ip, conn->remote_port, conn->local_port,
                        conn->snd_nxt, conn->rcv_nxt, flags, (uint16_t)rx_space,
                        data, data_len);
}

/* ------------------------------------------------------------------ */
/* Active open                                                         */
/* ------------------------------------------------------------------ */

tcp_conn_t* tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    (void)timeout_ms;
    tcp_conn_t* conn = tcp_alloc();
    if (!conn) return NULL;

    memset(conn, 0, sizeof(*conn));
    conn->in_use = true;
    conn->remote_ip = dst_ip;
    conn->remote_port = dst_port;

    // Skip any ephemeral port a listener owns: a connection out from
    // 49152 while something listens on 49152 would be ambiguous.
    for (;;) {
        conn->local_port = g_next_ephemeral_port++;
        if (g_next_ephemeral_port == 0) g_next_ephemeral_port = 49152;
        if (!tcp_is_listening(conn->local_port)) break;
    }

    conn->snd_nxt = (uint32_t)time_get_uptime_ms() * 1000u + 1;
    conn->snd_una = conn->snd_nxt;
    conn->state = TCP_SYN_SENT;
    conn->last_activity_ms = time_get_uptime_ms();

    for (int attempt = 0; attempt < TCP_MAX_RETRIES; attempt++) {
        if (!tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0)) {
            conn->in_use = false;
            return NULL;
        }

        uint64_t start = time_get_uptime_ms();
        while (time_get_uptime_ms() - start < TCP_RETRANSMIT_MS) {
            network_manager_poll();
            if (conn->state == TCP_ESTABLISHED) return conn;
            if (conn->reset) { conn->in_use = false; return NULL; }
            sleep(5);
        }
    }

    conn->in_use = false;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Passive open                                                        */
/* ------------------------------------------------------------------ */

static tcp_listener_t* tcp_find_listener(uint16_t port) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (g_listeners[i].used && g_listeners[i].port == port) return &g_listeners[i];
    }
    return NULL;
}

bool tcp_is_listening(uint16_t port) {
    return tcp_find_listener(port) != NULL;
}

bool tcp_listen(uint16_t port, uint32_t backlog) {
    if (port == 0) return false;
    if (tcp_find_listener(port)) return false;
    if (backlog == 0) backlog = 1;
    if (backlog > TCP_MAX_BACKLOG) backlog = TCP_MAX_BACKLOG;

    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (g_listeners[i].used) continue;
        g_listeners[i].used = true;
        g_listeners[i].port = port;
        g_listeners[i].backlog = backlog;
        return true;
    }
    return false;
}

void tcp_unlisten(uint16_t port) {
    tcp_listener_t* l = tcp_find_listener(port);
    if (!l) return;
    l->used = false;

    // Anything still waiting in the backlog was never handed to anyone,
    // so nobody holds a handle to it and nobody will ever close it.
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (c->in_use && c->accept_pending && c->local_port == port) {
            c->in_use = false;
            c->accept_pending = false;
            c->state = TCP_CLOSED;
        }
    }
}

/*
 * Frees passive connections that will never be accepted: handshakes the
 * peer abandoned, and completed connections the peer reset before we
 * got to them. Cheap (a table of TCP_MAX_CONNS), so it runs on every
 * accept and whenever a SYN finds the table full.
 */
static void tcp_reap_stale(void) {
    uint64_t now = time_get_uptime_ms();
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (!c->in_use) continue;

        bool stale = false;
        if (c->state == TCP_SYN_RCVD &&
            now - c->last_activity_ms > TCP_SYN_RCVD_TIMEOUT_MS) stale = true;
        if (c->accept_pending && c->state == TCP_CLOSED) stale = true;

        if (stale) {
            c->in_use = false;
            c->accept_pending = false;
            c->state = TCP_CLOSED;
        }
    }
}

static void tcp_passive_open(uint32_t src_ip, uint16_t remote_port,
                             uint16_t local_port, uint32_t peer_seq) {
    tcp_listener_t* l = tcp_find_listener(local_port);
    if (!l) return;

    // Backlog counts everything the listener has committed to but the
    // application has not taken: handshakes in flight plus connections
    // established and waiting. Past the limit the SYN is dropped, not
    // reset - the peer retransmits, and by then a slot may be free.
    uint32_t pending = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (c->in_use && c->local_port == local_port &&
            (c->state == TCP_SYN_RCVD || c->accept_pending)) pending++;
    }
    if (pending >= l->backlog) return;

    tcp_conn_t* c = tcp_alloc();
    if (!c) {
        tcp_reap_stale();
        c = tcp_alloc();
    }
    if (!c) return;

    memset(c, 0, sizeof(*c));
    uint32_t iss = (uint32_t)random_u64();

    c->in_use = true;
    c->state = TCP_SYN_RCVD;
    c->remote_ip = src_ip;
    c->remote_port = remote_port;
    c->local_port = local_port;
    c->rcv_nxt = peer_seq + 1;
    c->snd_una = iss;
    c->snd_nxt = iss;
    c->last_activity_ms = time_get_uptime_ms();
    c->accept_pending = true;
    c->accept_order = ++g_accept_seq;

    tcp_send_segment(c, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
    c->snd_nxt = iss + 1;       // the SYN occupies one sequence number
}

tcp_conn_t* tcp_accept(uint16_t port, uint32_t* out_remote_ip, uint16_t* out_remote_port) {
    tcp_reap_stale();

    tcp_conn_t* best = NULL;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (!c->in_use || !c->accept_pending || c->local_port != port) continue;
        if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) continue;

        // Ticket comparison survives the counter wrapping.
        if (!best || (int32_t)(c->accept_order - best->accept_order) < 0) best = c;
    }
    if (!best) return NULL;

    best->accept_pending = false;
    if (out_remote_ip) *out_remote_ip = best->remote_ip;
    if (out_remote_port) *out_remote_port = best->remote_port;
    return best;
}

/* ------------------------------------------------------------------ */
/* Data transfer                                                       */
/* ------------------------------------------------------------------ */

int32_t tcp_send(tcp_conn_t* conn, const void* data, uint32_t len, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!conn || !conn->in_use || conn->state != TCP_ESTABLISHED) return -1;
    if (len == 0) return 0;

    const uint8_t* p = (const uint8_t*)data;

    // Sequence number of data[0]. Offsets below are relative to it, so
    // "how much has been ACKed" is just snd_una - base, and a
    // retransmit is just "start sending again from acked_off".
    const uint32_t base = conn->snd_nxt;
    uint32_t sent_off = 0;      // next byte to put on the wire
    uint32_t acked_off = 0;     // bytes the peer has acknowledged
    int retries = 0;

    while (acked_off < len) {
        // Fill the window.
        while (sent_off < len && (sent_off - acked_off) < TCP_SEG_SIZE * TCP_SEND_WINDOW_SEGS) {
            uint32_t n = len - sent_off;
            if (n > TCP_SEG_SIZE) n = TCP_SEG_SIZE;

            conn->snd_nxt = base + sent_off;
            if (!tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, p + sent_off, (uint16_t)n)) {
                break;          // treated like a loss: the wait below times out and retries
            }
            sent_off += n;
        }

        // Wait for the ACK point to move.
        bool progressed = false;
        uint64_t start = time_get_uptime_ms();
        while (time_get_uptime_ms() - start < TCP_RETRANSMIT_MS) {
            network_manager_poll();

            int32_t d = (int32_t)(conn->snd_una - base);
            if (d < 0) d = 0;
            if ((uint32_t)d > len) d = (int32_t)len;
            if ((uint32_t)d > acked_off) {
                acked_off = (uint32_t)d;
                progressed = true;
                break;
            }

            if (conn->reset || conn->state == TCP_CLOSED) {
                conn->snd_nxt = base + acked_off;
                return (int32_t)acked_off;
            }
            sleep(1);
        }

        if (progressed) {
            retries = 0;
            if (sent_off < acked_off) sent_off = acked_off;
            continue;
        }

        if (++retries >= TCP_MAX_RETRIES) break;
        sent_off = acked_off;   // go-back-N: resend everything not yet ACKed
    }

    conn->snd_nxt = base + acked_off;
    return (int32_t)acked_off;
}

int32_t tcp_recv(tcp_conn_t* conn, void* buf, uint32_t maxlen) {
    if (!conn || !conn->in_use) return -1;
    network_manager_poll();

    if (conn->rx_len == 0) {
        if (conn->remote_closed || conn->reset) return -1;
        return 0;
    }

    uint32_t copy_len = conn->rx_len < maxlen ? conn->rx_len : maxlen;
    memcpy(buf, conn->rx_buf, copy_len);
    memmove(conn->rx_buf, conn->rx_buf + copy_len, conn->rx_len - copy_len);
    conn->rx_len -= copy_len;
    return (int32_t)copy_len;
}

bool tcp_is_closed(tcp_conn_t* conn) {
    return !conn || conn->state == TCP_CLOSED;
}

void tcp_close(tcp_conn_t* conn) {
    if (!conn || !conn->in_use) return;
    if (conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT) {
        tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        conn->snd_nxt++;
        conn->state = TCP_FIN_WAIT;

        uint64_t start = time_get_uptime_ms();
        while (time_get_uptime_ms() - start < 500) {
            network_manager_poll();
            if (conn->state == TCP_CLOSED) break;
            sleep(5);
        }
    }
    conn->in_use = false;
    conn->accept_pending = false;
    conn->state = TCP_CLOSED;
}

void tcp_abort(tcp_conn_t* conn) {
    if (!conn || !conn->in_use) return;
    if (conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT ||
        conn->state == TCP_FIN_WAIT || conn->state == TCP_SYN_RCVD) {
        tcp_send_segment(conn, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0);
    }
    conn->in_use = false;
    conn->accept_pending = false;
    conn->state = TCP_CLOSED;
}

/* ------------------------------------------------------------------ */
/* Receive path                                                        */
/* ------------------------------------------------------------------ */

/*
 * RFC 793 reset generation for a segment that matches no connection.
 * Without this a SYN to a closed port is silently dropped and the peer
 * spends its whole connect timeout finding out; and a peer talking to a
 * connection this side has already forgotten (a server process that was
 * killed, whose handle the kernel then swept) keeps retransmitting into
 * the void.
 */
static void tcp_reply_rst(uint32_t src_ip, const tcp_header_t* hdr, uint16_t data_len) {
    if (hdr->flags & TCP_FLAG_RST) return;      // never answer a RST with a RST
    if (!ip_is_configured()) return;

    uint16_t local_port = net_ntohs(hdr->dst_port);
    uint16_t remote_port = net_ntohs(hdr->src_port);

    if (hdr->flags & TCP_FLAG_ACK) {
        tcp_send_raw(src_ip, remote_port, local_port,
                     net_ntohl(hdr->ack), 0, TCP_FLAG_RST, 0, NULL, 0);
    } else {
        uint32_t seg_len = data_len;
        if (hdr->flags & TCP_FLAG_SYN) seg_len++;
        if (hdr->flags & TCP_FLAG_FIN) seg_len++;
        tcp_send_raw(src_ip, remote_port, local_port,
                     0, net_ntohl(hdr->seq) + seg_len,
                     TCP_FLAG_RST | TCP_FLAG_ACK, 0, NULL, 0);
    }
}

void tcp_handle_packet(uint32_t src_ip, const uint8_t* seg, uint16_t len) {
    if (len < sizeof(tcp_header_t)) return;
    const tcp_header_t* hdr = (const tcp_header_t*)seg;
    uint16_t local_port = net_ntohs(hdr->dst_port);
    uint16_t remote_port = net_ntohs(hdr->src_port);

    uint8_t data_off = (uint8_t)((hdr->data_offset >> 4) * 4);
    if (data_off < sizeof(tcp_header_t) || data_off > len) return;
    const uint8_t* data = seg + data_off;
    uint16_t data_len = len - data_off;

    uint32_t seq = net_ntohl(hdr->seq);
    uint32_t ack = net_ntohl(hdr->ack);

    tcp_conn_t* conn = tcp_find(src_ip, remote_port, local_port);
    if (!conn) {
        if ((hdr->flags & TCP_FLAG_SYN) && !(hdr->flags & TCP_FLAG_ACK) &&
            tcp_is_listening(local_port)) {
            tcp_passive_open(src_ip, remote_port, local_port, seq);
            return;
        }
        tcp_reply_rst(src_ip, hdr, data_len);
        return;
    }

    conn->last_activity_ms = time_get_uptime_ms();

    if (conn->state == TCP_SYN_RCVD) {
        // Nobody holds a handle to a connection that has not been
        // accepted, so a reset here frees the slot outright instead of
        // leaving it for an owner to close.
        if (hdr->flags & TCP_FLAG_RST) {
            conn->in_use = false;
            conn->accept_pending = false;
            conn->state = TCP_CLOSED;
            return;
        }

        // The peer never saw our SYN|ACK and is retrying its SYN.
        if ((hdr->flags & TCP_FLAG_SYN) && !(hdr->flags & TCP_FLAG_ACK)) {
            conn->snd_nxt = conn->snd_una;
            tcp_send_segment(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
            conn->snd_nxt = conn->snd_una + 1;
            return;
        }

        // Third step of the handshake. Anything else is noise.
        if (!(hdr->flags & TCP_FLAG_ACK) || ack != conn->snd_nxt) return;
        conn->snd_una = ack;
        conn->state = TCP_ESTABLISHED;
        // No return: this segment may already carry the peer's first
        // bytes (or a FIN), and the code below handles both.
    }

    if (hdr->flags & TCP_FLAG_RST) {
        conn->reset = true;
        conn->state = TCP_CLOSED;
        return;
    }

    if (conn->state == TCP_SYN_SENT) {
        if ((hdr->flags & TCP_FLAG_SYN) && (hdr->flags & TCP_FLAG_ACK)) {
            conn->rcv_nxt = seq + 1;
            conn->snd_una = ack;
            if (conn->snd_nxt < ack) conn->snd_nxt = ack;
            conn->state = TCP_ESTABLISHED;
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
        }
        return;
    }

    if (hdr->flags & TCP_FLAG_ACK) {
        if (ack > conn->snd_una) conn->snd_una = ack;
    }

    if (data_len > 0 && seq == conn->rcv_nxt) {
        uint32_t space = TCP_RX_BUF_SIZE - conn->rx_len;
        uint32_t copy_len = data_len < space ? data_len : space;
        memcpy(conn->rx_buf + conn->rx_len, data, copy_len);
        conn->rx_len += copy_len;
        conn->rcv_nxt += copy_len;
        tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
    } else if (data_len > 0) {
        // Out-of-order/duplicate - no reassembly buffer in this simplified
        // stack; just re-ACK our current rcv_nxt.
        tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
    }

    if (hdr->flags & TCP_FLAG_FIN) {
        conn->rcv_nxt = seq + data_len + 1;
        conn->remote_closed = true;
        tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
        if (conn->state == TCP_ESTABLISHED) conn->state = TCP_CLOSE_WAIT;
        else if (conn->state == TCP_FIN_WAIT) conn->state = TCP_CLOSED;
    } else if ((hdr->flags & TCP_FLAG_ACK) && conn->state == TCP_FIN_WAIT &&
               conn->snd_una == conn->snd_nxt) {
        conn->state = TCP_CLOSED;
    }
}
