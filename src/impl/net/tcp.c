#include "net/tcp.h"
#include "net/ip.h"
#include "x86_64/network_manager.h"
#include "time.h"
#include "string.h"
#include "serial.h"

#define TCP_MAX_CONNS 4
#define TCP_DEFAULT_WINDOW 4096
#define TCP_RETRANSMIT_MS 500
#define TCP_MAX_RETRIES 6

static tcp_conn_t g_conns[TCP_MAX_CONNS];
static uint16_t g_next_ephemeral_port = 49152;

void tcp_init(void) {
    memset(g_conns, 0, sizeof(g_conns));
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

static bool tcp_send_segment(tcp_conn_t* conn, uint8_t flags, const void* data, uint16_t data_len) {
    static uint8_t buf[ETH_MTU];
    tcp_header_t* hdr = (tcp_header_t*)buf;
    hdr->src_port = net_htons(conn->local_port);
    hdr->dst_port = net_htons(conn->remote_port);
    hdr->seq = net_htonl(conn->snd_nxt);
    hdr->ack = net_htonl((flags & TCP_FLAG_ACK) ? conn->rcv_nxt : 0);
    hdr->data_offset = (uint8_t)(5 << 4);
    hdr->flags = flags;
    hdr->window = net_htons(TCP_DEFAULT_WINDOW);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data_len > sizeof(buf) - sizeof(tcp_header_t)) return false;
    if (data && data_len) memcpy(buf + sizeof(tcp_header_t), data, data_len);

    uint16_t seg_len = (uint16_t)(sizeof(tcp_header_t) + data_len);
    hdr->checksum = net_htons(net_tcp_checksum(ip_get_local(), conn->remote_ip, buf, seg_len));

    return ip_send(conn->remote_ip, IP_PROTO_TCP, buf, seg_len);
}

tcp_conn_t* tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    tcp_conn_t* conn = tcp_alloc();
    if (!conn) return NULL;

    memset(conn, 0, sizeof(*conn));
    conn->in_use = true;
    conn->remote_ip = dst_ip;
    conn->remote_port = dst_port;
    conn->local_port = g_next_ephemeral_port++;
    if (g_next_ephemeral_port == 0) g_next_ephemeral_port = 49152;

    conn->snd_nxt = (uint32_t)time_get_uptime_ms() * 1000u + 1;
    conn->snd_una = conn->snd_nxt;
    conn->state = TCP_SYN_SENT;
    conn->last_activity_ms = time_get_uptime_ms();

    for (int attempt = 0; attempt < TCP_MAX_RETRIES; attempt++) {
        tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0);

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

int32_t tcp_send(tcp_conn_t* conn, const void* data, uint32_t len, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!conn || !conn->in_use || conn->state != TCP_ESTABLISHED) return -1;

    const uint8_t* p = (const uint8_t*)data;
    uint32_t sent = 0;
    const uint32_t CHUNK = 1024;

    while (sent < len) {
        uint32_t this_chunk = len - sent;
        if (this_chunk > CHUNK) this_chunk = CHUNK;

        uint32_t seq_before = conn->snd_nxt;
        conn->snd_nxt += this_chunk;

        bool acked = false;
        for (int attempt = 0; attempt < TCP_MAX_RETRIES && !acked; attempt++) {
            tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, p + sent, (uint16_t)this_chunk);

            uint64_t start = time_get_uptime_ms();
            while (time_get_uptime_ms() - start < TCP_RETRANSMIT_MS) {
                network_manager_poll();
                if (conn->snd_una >= seq_before + this_chunk) { acked = true; break; }
                if (conn->reset || conn->state == TCP_CLOSED) return (int32_t)sent;
                sleep(5);
            }
        }
        if (!acked) return (int32_t)sent;
        sent += this_chunk;
    }
    return (int32_t)sent;
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
    conn->state = TCP_CLOSED;
}

void tcp_handle_packet(uint32_t src_ip, const uint8_t* seg, uint16_t len) {
    if (len < sizeof(tcp_header_t)) return;
    const tcp_header_t* hdr = (const tcp_header_t*)seg;
    uint16_t local_port = net_ntohs(hdr->dst_port);
    uint16_t remote_port = net_ntohs(hdr->src_port);

    tcp_conn_t* conn = tcp_find(src_ip, remote_port, local_port);
    if (!conn) return;

    uint8_t data_off = (uint8_t)((hdr->data_offset >> 4) * 4);
    if (data_off < sizeof(tcp_header_t) || data_off > len) return;
    const uint8_t* data = seg + data_off;
    uint16_t data_len = len - data_off;

    uint32_t seq = net_ntohl(hdr->seq);
    uint32_t ack = net_ntohl(hdr->ack);

    conn->last_activity_ms = time_get_uptime_ms();

    if (hdr->flags & TCP_FLAG_RST) {
        conn->reset = true;
        conn->state = TCP_CLOSED;
        return;
    }

    if (conn->state == TCP_SYN_SENT) {
        if ((hdr->flags & TCP_FLAG_SYN) && (hdr->flags & TCP_FLAG_ACK)) {
            conn->rcv_nxt = seq + 1;
            conn->snd_una = ack;
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
