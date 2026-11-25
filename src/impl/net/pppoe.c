#include "net/pppoe.h"
#include "net/ip.h"
#include "net/ethernet.h"
#include "x86_64/network_manager.h"
#include "x86_64/net_syscall.h"
#include "x86_64/scheduler.h"
#include "x86_64/proc.h"
#include "time.h"
#include "string.h"
#include "serial.h"

/*
 * PPPoE client (RFC 2516) with LCP, PAP / CHAP-MD5 and IPCP (RFC 1661,
 * 1334, 1994, 1332, 1877): what DSL and many fibre lines use when the
 * computer is plugged straight into the ISP's modem or ONT.
 *
 * Received frames are handled in the receive path (answers to the ISP's
 * requests go out at once); a background task sends our own requests,
 * retransmits them, keeps the session alive with LCP echoes and
 * reconnects when the session ends.
 */

#define ETHERTYPE_PPPOE_DISC 0x8863
#define ETHERTYPE_PPPOE_SESS 0x8864

#define PADI 0x09
#define PADO 0x07
#define PADR 0x19
#define PADS 0x65
#define PADT 0xA7

#define TAG_END          0x0000
#define TAG_SERVICE_NAME 0x0101
#define TAG_HOST_UNIQ    0x0103
#define TAG_AC_COOKIE    0x0104
#define TAG_RELAY_ID     0x0110
#define TAG_SERVICE_ERR  0x0201
#define TAG_SYSTEM_ERR   0x0202
#define TAG_GENERIC_ERR  0x0203

#define PPP_IP    0x0021
#define PPP_IPCP  0x8021
#define PPP_LCP   0xC021
#define PPP_PAP   0xC023
#define PPP_CHAP  0xC223

enum { CONF_REQ = 1, CONF_ACK, CONF_NAK, CONF_REJ, TERM_REQ, TERM_ACK, CODE_REJ, PROTO_REJ,
       ECHO_REQ, ECHO_REPLY, DISCARD_REQ };

#define PPPOE_MTU 1492

enum { PS_IDLE, PS_DISCOVERY, PS_REQUEST, PS_LCP, PS_AUTH, PS_IPCP, PS_UP };

static struct {
    volatile int state;
    char user[64], password[64];
    uint8_t ac_mac[6];
    uint16_t session;
    uint8_t cookie[64], relay[64];
    uint8_t cookie_len, relay_len;
    uint8_t host_uniq[4];
    uint32_t magic;
    /* LCP */
    bool lcp_ours_acked, lcp_peers_acked;
    bool send_mru;
    uint16_t mru;
    uint16_t auth;          /* protocol the ISP asked for, 0: none */
    uint8_t id;             /* our next packet identifier */
    uint8_t lcp_req_id, auth_req_id, ipcp_req_id;
    volatile bool auth_done, auth_failed;
    /* IPCP */
    bool ipcp_ours_acked, ipcp_peers_acked;
    bool want_dns1, want_dns2;
    uint32_t our_ip, peer_ip, dns1, dns2;
    volatile uint64_t last_rx_ms;
    volatile bool resend_now;   /* a NAK changed our request */
    char last_error[96];
} P;

static volatile bool g_wanted;
static volatile bool g_task_running;

/* ---------------- MD5 (RFC 1321), for CHAP ---------------- */

static uint32_t md5_rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5(const uint8_t* msg, uint32_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint8_t R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    uint8_t buf[448];               /* CHAP input is at most 1 + 63 + 255 bytes */
    uint32_t total = ((len + 8) / 64 + 1) * 64;
    if (total > sizeof(buf)) total = sizeof(buf), len = total - 9;
    memset(buf, 0, total);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 8 + i] = (uint8_t)(bits >> (8 * i));
    for (uint32_t off = 0; off < total; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)buf[off + i * 4] | (uint32_t)buf[off + i * 4 + 1] << 8 |
                   (uint32_t)buf[off + i * 4 + 2] << 16 | (uint32_t)buf[off + i * 4 + 3] << 24;
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = (uint32_t)i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (uint32_t)(5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (uint32_t)(3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (uint32_t)(7 * i) % 16; }
            uint32_t t = d;
            d = c;
            c = b;
            b = b + md5_rol(a + f + K[i] + w[g], R[i]);
            a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    uint32_t h[4] = {h0, h1, h2, h3};
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
}

/* ---------------- sending ---------------- */

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* discovery packet with the usual tags */
static void send_discovery(uint8_t code, const uint8_t dst[6]) {
    uint8_t pkt[200];
    uint8_t* t = pkt + 6;
    put16(t, TAG_SERVICE_NAME); put16(t + 2, 0); t += 4;
    put16(t, TAG_HOST_UNIQ); put16(t + 2, 4); memcpy(t + 4, P.host_uniq, 4); t += 8;
    if (code == PADR && P.cookie_len) {
        put16(t, TAG_AC_COOKIE); put16(t + 2, P.cookie_len); memcpy(t + 4, P.cookie, P.cookie_len);
        t += 4 + P.cookie_len;
    }
    if (code == PADR && P.relay_len) {
        put16(t, TAG_RELAY_ID); put16(t + 2, P.relay_len); memcpy(t + 4, P.relay, P.relay_len);
        t += 4 + P.relay_len;
    }
    pkt[0] = 0x11;
    pkt[1] = code;
    put16(pkt + 2, code == PADT ? P.session : 0);
    put16(pkt + 4, (uint16_t)(t - pkt - 6));
    eth_send(dst, ETHERTYPE_PPPOE_DISC, pkt, (uint16_t)(t - pkt));
}

/* a PPP frame inside the session */
static void send_ppp(uint16_t proto, const uint8_t* data, uint16_t len) {
    static uint8_t pkt[ETH_MTU];
    if (len + 8 > sizeof(pkt)) return;
    pkt[0] = 0x11;
    pkt[1] = 0;
    put16(pkt + 2, P.session);
    put16(pkt + 4, (uint16_t)(len + 2));
    put16(pkt + 6, proto);
    memcpy(pkt + 8, data, len);
    eth_send(P.ac_mac, ETHERTYPE_PPPOE_SESS, pkt, (uint16_t)(len + 8));
}

static void send_control(uint16_t proto, uint8_t code, uint8_t id, const uint8_t* body, uint16_t len) {
    uint8_t pkt[600];
    if (len + 4 > sizeof(pkt)) return;
    pkt[0] = code;
    pkt[1] = id;
    put16(pkt + 2, (uint16_t)(len + 4));
    if (len) memcpy(pkt + 4, body, len);
    send_ppp(proto, pkt, (uint16_t)(len + 4));
}

static void send_lcp_request(void) {
    uint8_t o[16];
    uint16_t n = 0;
    if (P.send_mru) { o[n++] = 1; o[n++] = 4; put16(o + n, P.mru); n += 2; }
    o[n++] = 5; o[n++] = 6;
    for (int i = 0; i < 4; i++) o[n++] = (uint8_t)(P.magic >> (24 - 8 * i));
    P.lcp_req_id = ++P.id;
    send_control(PPP_LCP, CONF_REQ, P.lcp_req_id, o, n);
}

static void send_ipcp_request(void) {
    uint8_t o[18];
    uint16_t n = 0;
    o[n++] = 3; o[n++] = 6;
    for (int i = 0; i < 4; i++) o[n++] = (uint8_t)(P.our_ip >> (24 - 8 * i));
    if (P.want_dns1) { o[n++] = 129; o[n++] = 6; for (int i = 0; i < 4; i++) o[n++] = (uint8_t)(P.dns1 >> (24 - 8 * i)); }
    if (P.want_dns2) { o[n++] = 131; o[n++] = 6; for (int i = 0; i < 4; i++) o[n++] = (uint8_t)(P.dns2 >> (24 - 8 * i)); }
    P.ipcp_req_id = ++P.id;
    send_control(PPP_IPCP, CONF_REQ, P.ipcp_req_id, o, n);
}

static void send_pap(void) {
    uint8_t body[132];
    uint8_t ul = (uint8_t)strlen(P.user), pl = (uint8_t)strlen(P.password);
    body[0] = ul;
    memcpy(body + 1, P.user, ul);
    body[1 + ul] = pl;
    memcpy(body + 2 + ul, P.password, pl);
    P.auth_req_id = ++P.id;
    send_control(PPP_PAP, 1, P.auth_req_id, body, (uint16_t)(2 + ul + pl));
}

static void set_error(const char* text) {
    snprintf(P.last_error, sizeof(P.last_error), "%s", text);
    serial_write_str("PPPoE: ");
    serial_write_str(text);
    serial_write_str("\n");
}

static void session_down(const char* why) {
    if (P.state == PS_UP && ip_get_local() == P.our_ip) {
        ip_set_link(NULL);
        ip_configure(0, 0, 0);
        ip_set_dns(0);
    }
    if (why) set_error(why);
    P.state = PS_IDLE;
}

/* ---------------- receiving ---------------- */

static void link_up(void) {
    P.state = PS_UP;
    ip_set_link(pppoe_send_ip);
    ip_configure(P.our_ip, 0xFFFFFFFFu, P.peer_ip);
    ip_set_dns(P.dns1 ? P.dns1 : P.dns2);
    char ip[16], gw[16], line[96];
    ip_to_string(P.our_ip, ip);
    ip_to_string(P.peer_ip, gw);
    snprintf(line, sizeof(line), "PPPoE: connected, address %s, peer %s\n", ip, gw);
    serial_write_str(line);
    P.last_error[0] = 0;
}

static void maybe_advance(void) {
    if (P.state == PS_LCP && P.lcp_ours_acked && P.lcp_peers_acked) {
        P.state = P.auth ? PS_AUTH : PS_IPCP;
        P.auth_done = P.auth_failed = false;
        if (P.auth == PPP_PAP) send_pap();
        if (P.state == PS_IPCP) send_ipcp_request();
    }
    if (P.state == PS_AUTH && P.auth_done) {
        P.state = PS_IPCP;
        send_ipcp_request();
    }
    if (P.state == PS_IPCP && P.ipcp_ours_acked && P.ipcp_peers_acked && P.our_ip) link_up();
}

/* The ISP's LCP Configure-Request: ack, nak or reject its options. */
static void lcp_peer_request(uint8_t id, const uint8_t* o, uint16_t len) {
    uint8_t rej[300], nak[64];
    uint16_t rn = 0, nn = 0;
    uint16_t auth = 0;
    for (uint16_t i = 0; i + 2 <= len;) {
        uint8_t type = o[i], ol = o[i + 1];
        if (ol < 2 || i + ol > len) break;
        const uint8_t* v = o + i + 2;
        if (type == 1 && ol == 4) {
            if (get16(v) < 576 && nn + 4 <= sizeof(nak)) { nak[nn++] = 1; nak[nn++] = 4; put16(nak + nn, PPPOE_MTU); nn += 2; }
        } else if (type == 3 && ol >= 4) {
            uint16_t proto = get16(v);
            if (proto == PPP_PAP && ol == 4) auth = PPP_PAP;
            else if (proto == PPP_CHAP && ol == 5 && v[2] == 5) auth = PPP_CHAP;
            else if (nn + 5 <= sizeof(nak)) {   /* MS-CHAP, EAP, ...: suggest CHAP-MD5 */
                nak[nn++] = 3; nak[nn++] = 5; put16(nak + nn, PPP_CHAP); nn += 2; nak[nn++] = 5;
            }
        } else if (type == 5 && ol == 6) {
            /* magic number: fine */
        } else if (rn + ol <= sizeof(rej)) {
            memcpy(rej + rn, o + i, ol);   /* PFC, ACFC, multilink, callback, ... */
            rn += ol;
        }
        i += ol;
    }
    if (rn) {
        send_control(PPP_LCP, CONF_REJ, id, rej, rn);
    } else if (nn) {
        send_control(PPP_LCP, CONF_NAK, id, nak, nn);
    } else {
        send_control(PPP_LCP, CONF_ACK, id, o, len);
        P.auth = auth;
        P.lcp_peers_acked = true;
        maybe_advance();
    }
}

static void handle_lcp(const uint8_t* p, uint16_t len) {
    if (len < 4) return;
    uint8_t code = p[0], id = p[1];
    uint16_t plen = get16(p + 2);
    if (plen > len || plen < 4) return;
    const uint8_t* body = p + 4;
    uint16_t blen = (uint16_t)(plen - 4);
    switch (code) {
    case CONF_REQ:
        if (P.state >= PS_AUTH) {   /* renegotiation: start the link again */
            if (P.state == PS_UP) session_down("the ISP restarted the link");
            P.state = PS_LCP;
            P.lcp_ours_acked = P.lcp_peers_acked = false;
            P.ipcp_ours_acked = P.ipcp_peers_acked = false;
            send_lcp_request();
        }
        lcp_peer_request(id, body, blen);
        break;
    case CONF_ACK:
        if (id == P.lcp_req_id) { P.lcp_ours_acked = true; maybe_advance(); }
        break;
    case CONF_NAK:
    case CONF_REJ:
        if (id != P.lcp_req_id) break;
        for (uint16_t i = 0; i + 2 <= blen;) {
            uint8_t type = body[i], ol = body[i + 1];
            if (ol < 2 || i + ol > blen) break;
            if (type == 1) {
                if (code == CONF_REJ) P.send_mru = false;
                else if (ol == 4) P.mru = get16(body + i + 2) < PPPOE_MTU ? get16(body + i + 2) : PPPOE_MTU;
            }
            if (type == 5) P.magic = P.magic * 1103515245u + 12345u;
            i += ol;
        }
        P.resend_now = true;
        break;
    case TERM_REQ:
        send_control(PPP_LCP, TERM_ACK, id, NULL, 0);
        session_down("the ISP ended the session");
        break;
    case ECHO_REQ: {
        uint8_t reply[64];
        uint16_t n = blen > sizeof(reply) ? sizeof(reply) : blen;
        memcpy(reply, body, n);
        if (n >= 4) for (int i = 0; i < 4; i++) reply[i] = (uint8_t)(P.magic >> (24 - 8 * i));
        send_control(PPP_LCP, ECHO_REPLY, id, reply, n);
        break;
    }
    default:
        break;   /* echo replies only refresh last_rx_ms */
    }
}

static void handle_pap(const uint8_t* p, uint16_t len) {
    if (len < 4 || P.state != PS_AUTH) return;
    if (p[0] == 2) { P.auth_done = true; maybe_advance(); }
    else if (p[0] == 3) { P.auth_failed = true; }
}

static void handle_chap(const uint8_t* p, uint16_t len) {
    if (len < 4) return;
    uint8_t code = p[0], id = p[1];
    if (code == 1 && len >= 5) {   /* challenge */
        uint8_t vlen = p[4];
        if (5u + vlen > len) return;
        uint8_t input[1 + 64 + 255];
        uint8_t pl = (uint8_t)strlen(P.password);
        input[0] = id;
        memcpy(input + 1, P.password, pl);
        memcpy(input + 1 + pl, p + 5, vlen);
        uint8_t digest[16];
        md5(input, (uint32_t)(1 + pl + vlen), digest);
        uint8_t body[1 + 16 + 64];
        uint8_t ul = (uint8_t)strlen(P.user);
        body[0] = 16;
        memcpy(body + 1, digest, 16);
        memcpy(body + 17, P.user, ul);
        send_control(PPP_CHAP, 2, id, body, (uint16_t)(17 + ul));
    } else if (code == 3) {
        P.auth_done = true;
        maybe_advance();
    } else if (code == 4) {
        P.auth_failed = true;
    }
}

static void handle_ipcp(const uint8_t* p, uint16_t len) {
    if (len < 4) return;
    uint8_t code = p[0], id = p[1];
    uint16_t plen = get16(p + 2);
    if (plen > len || plen < 4) return;
    const uint8_t* o = p + 4;
    uint16_t olen = (uint16_t)(plen - 4);
    if (code == CONF_REQ) {
        uint8_t rej[64];
        uint16_t rn = 0;
        for (uint16_t i = 0; i + 2 <= olen;) {
            uint8_t type = o[i], ol = o[i + 1];
            if (ol < 2 || i + ol > olen) break;
            if (type == 3 && ol == 6) P.peer_ip = get32(o + i + 2);
            else if (rn + ol <= sizeof(rej)) { memcpy(rej + rn, o + i, ol); rn += ol; }
            i += ol;
        }
        if (rn) {
            send_control(PPP_IPCP, CONF_REJ, id, rej, rn);
        } else {
            send_control(PPP_IPCP, CONF_ACK, id, o, olen);
            P.ipcp_peers_acked = true;
            maybe_advance();
        }
        return;
    }
    if (id != P.ipcp_req_id) return;
    if (code == CONF_ACK) {
        P.ipcp_ours_acked = true;
        maybe_advance();
    } else if (code == CONF_NAK || code == CONF_REJ) {
        for (uint16_t i = 0; i + 2 <= olen;) {
            uint8_t type = o[i], ol = o[i + 1];
            if (ol < 2 || i + ol > olen) break;
            uint32_t v = ol == 6 ? get32(o + i + 2) : 0;
            if (code == CONF_NAK) {
                if (type == 3) P.our_ip = v;
                if (type == 129) P.dns1 = v;
                if (type == 131) P.dns2 = v;
            } else {
                if (type == 129) P.want_dns1 = false;
                if (type == 131) P.want_dns2 = false;
            }
            i += ol;
        }
        send_ipcp_request();
    }
}

void pppoe_handle_frame(const uint8_t src_mac[6], uint16_t ethertype, const uint8_t* p, uint16_t len) {
    if (len < 6 || p[0] != 0x11 || P.state == PS_IDLE) return;
    uint8_t code = p[1];
    uint16_t session = get16(p + 2);
    uint16_t plen = get16(p + 4);
    if (plen + 6u > len) return;
    const uint8_t* payload = p + 6;

    if (ethertype == ETHERTYPE_PPPOE_DISC) {
        /* tags */
        bool ours = false;
        uint8_t cookie_len = 0, relay_len = 0;
        const uint8_t* cookie = NULL;
        const uint8_t* relay = NULL;
        const char* error = NULL;
        for (uint16_t i = 0; i + 4 <= plen;) {
            uint16_t tag = get16(payload + i), tl = get16(payload + i + 2);
            if (i + 4u + tl > plen) break;
            const uint8_t* v = payload + i + 4;
            if (tag == TAG_HOST_UNIQ && tl == 4 && !memcmp(v, P.host_uniq, 4)) ours = true;
            if (tag == TAG_AC_COOKIE && tl <= sizeof(P.cookie)) { cookie = v; cookie_len = (uint8_t)tl; }
            if (tag == TAG_RELAY_ID && tl <= sizeof(P.relay)) { relay = v; relay_len = (uint8_t)tl; }
            if (tag == TAG_SERVICE_ERR) error = "the ISP does not offer the requested service";
            if (tag == TAG_SYSTEM_ERR || tag == TAG_GENERIC_ERR) error = "the ISP's access concentrator reported an error";
            if (tag == TAG_END) break;
            i += 4 + tl;
        }
        if (code == PADO && P.state == PS_DISCOVERY && ours && !error) {
            memcpy(P.ac_mac, src_mac, 6);
            P.cookie_len = cookie_len;
            if (cookie) memcpy(P.cookie, cookie, cookie_len);
            P.relay_len = relay_len;
            if (relay) memcpy(P.relay, relay, relay_len);
            P.state = PS_REQUEST;
            send_discovery(PADR, P.ac_mac);
        } else if (code == PADS && P.state == PS_REQUEST && ours) {
            if (error || !session) {
                set_error(error ? error : "the ISP refused the session");
                P.state = PS_IDLE;
                return;
            }
            P.session = session;
            P.state = PS_LCP;
            P.lcp_ours_acked = P.lcp_peers_acked = false;
            P.last_rx_ms = time_get_uptime_ms();
            send_lcp_request();
        } else if (code == PADT && session == P.session && !memcmp(src_mac, P.ac_mac, 6)) {
            session_down("the ISP closed the session");
        }
        return;
    }

    /* session data */
    if (P.state < PS_LCP || session != P.session || memcmp(src_mac, P.ac_mac, 6) || code != 0 || plen < 2)
        return;
    P.last_rx_ms = time_get_uptime_ms();
    uint16_t proto = get16(payload);
    const uint8_t* data = payload + 2;
    uint16_t dlen = (uint16_t)(plen - 2);
    switch (proto) {
    case PPP_IP:
        if (P.state == PS_UP) ip_handle_packet(data, dlen);
        break;
    case PPP_LCP: handle_lcp(data, dlen); break;
    case PPP_PAP: handle_pap(data, dlen); break;
    case PPP_CHAP: handle_chap(data, dlen); break;
    case PPP_IPCP: if (P.state >= PS_IPCP) handle_ipcp(data, dlen); break;
    default: {
        /* IPv6CP, CCP, ...: LCP Protocol-Reject with the start of the packet */
        uint8_t body[2 + 64];
        uint16_t n = dlen > 64 ? 64 : dlen;
        put16(body, proto);
        memcpy(body + 2, data, n);
        send_control(PPP_LCP, PROTO_REJ, ++P.id, body, (uint16_t)(2 + n));
        break;
    }
    }
}

bool pppoe_send_ip(const void* packet, uint16_t len) {
    if (P.state != PS_UP || len > P.mru) return false;
    send_ppp(PPP_IP, (const uint8_t*)packet, len);
    return true;
}

/* ---------------- background task ---------------- */

static void reset_session(void) {
    uint64_t now = time_get_uptime_ms();
    for (int i = 0; i < 4; i++) P.host_uniq[i] = (uint8_t)(now >> (8 * i)) ^ g_local_mac[2 + i];
    P.magic = (uint32_t)(now * 2654435761u) ^ 0x50505050u;
    P.session = 0;
    P.cookie_len = P.relay_len = 0;
    P.send_mru = true;
    P.mru = PPPOE_MTU;
    P.auth = 0;
    P.lcp_ours_acked = P.lcp_peers_acked = false;
    P.ipcp_ours_acked = P.ipcp_peers_acked = false;
    P.want_dns1 = P.want_dns2 = true;
    P.our_ip = P.peer_ip = P.dns1 = P.dns2 = 0;
    P.auth_done = P.auth_failed = false;
}

/* sends under the stack claim (the frame buffers are shared) */
#define LOCKED(stmt) do { if (net_stack_claim()) { stmt; net_stack_release(); } } while (0)

static bool wait_state_change(int from, uint32_t ms) {
    uint64_t start = time_get_uptime_ms();
    while (time_get_uptime_ms() - start < ms) {
        if (P.state != from || P.resend_now || !g_wanted) return true;
        sleep(20);
    }
    return false;
}

static void pppoe_task(void) {
    uint32_t backoff_ms = 2000;
    for (;;) {
        if (!g_wanted) {
            if (P.state != PS_IDLE) {
                LOCKED(if (P.state >= PS_LCP) {
                    send_control(PPP_LCP, TERM_REQ, ++P.id, NULL, 0);
                    send_discovery(PADT, P.ac_mac);
                });
                session_down(NULL);
            }
            sleep(500);
            continue;
        }
        int s = P.state;
        if (s == PS_IDLE) {
            reset_session();
            P.state = PS_DISCOVERY;
            continue;
        }
        if (s == PS_DISCOVERY) {
            LOCKED(send_discovery(PADI, BROADCAST));
            if (!wait_state_change(PS_DISCOVERY, backoff_ms)) {
                if (backoff_ms < 16000) backoff_ms *= 2;
                if (backoff_ms >= 8000) set_error("no answer from the ISP (is the modem in bridge mode, is a VLAN needed?)");
            }
            continue;
        }
        backoff_ms = 2000;
        if (s == PS_REQUEST) {
            for (int i = 0; i < 4 && P.state == PS_REQUEST; i++) {
                if (i) LOCKED(send_discovery(PADR, P.ac_mac));
                wait_state_change(PS_REQUEST, 3000);
            }
            if (P.state == PS_REQUEST) P.state = PS_IDLE;
            continue;
        }
        if (s == PS_LCP || s == PS_AUTH || s == PS_IPCP) {
            /* retransmit our current request every 3 s, give up after 30 s */
            uint64_t start = time_get_uptime_ms();
            while (P.state == s && g_wanted && time_get_uptime_ms() - start < 30000) {
                P.resend_now = false;
                if (!wait_state_change(s, 3000) || P.resend_now) {
                    if (P.state != s) break;
                    LOCKED(
                        if (s == PS_LCP && !P.lcp_ours_acked) send_lcp_request();
                        else if (s == PS_AUTH && P.auth == PPP_PAP) send_pap();
                        else if (s == PS_IPCP && !P.ipcp_ours_acked) send_ipcp_request());
                }
                if (P.auth_failed) break;
            }
            if (P.auth_failed) {
                set_error("the ISP rejected the user name or password");
                LOCKED(send_discovery(PADT, P.ac_mac));
                P.state = PS_IDLE;
                sleep(10000);
            } else if (P.state == s) {
                set_error(s == PS_LCP ? "link negotiation timed out" :
                          s == PS_AUTH ? "login timed out" : "address negotiation timed out");
                LOCKED(send_discovery(PADT, P.ac_mac));
                P.state = PS_IDLE;
            }
            continue;
        }
        if (s == PS_UP) {
            /* keepalive: an LCP echo every 15 s, down after 60 s of silence */
            sleep(1000);
            uint64_t idle = time_get_uptime_ms() - P.last_rx_ms;
            if (idle > 60000) {
                LOCKED(send_discovery(PADT, P.ac_mac));
                session_down("the ISP stopped answering");
            } else if (idle > 15000 && (time_get_uptime_ms() / 1000) % 15 == 0) {
                uint8_t magic[4];
                for (int i = 0; i < 4; i++) magic[i] = (uint8_t)(P.magic >> (24 - 8 * i));
                LOCKED(send_control(PPP_LCP, ECHO_REQ, ++P.id, magic, 4));
            }
        }
    }
}

bool pppoe_start(const char* user, const char* password) {
    if (!user || !password || strlen(user) >= sizeof(P.user) || strlen(password) >= sizeof(P.password))
        return false;
    if (g_wanted && P.state != PS_IDLE) {
        g_wanted = false;
        uint64_t start = time_get_uptime_ms();
        while (P.state != PS_IDLE && time_get_uptime_ms() - start < 3000) sleep(50);
    }
    snprintf(P.user, sizeof(P.user), "%s", user);
    snprintf(P.password, sizeof(P.password), "%s", password);
    P.last_error[0] = 0;
    g_wanted = true;
    if (!__atomic_exchange_n(&g_task_running, true, __ATOMIC_ACQ_REL))
        if (!proc_create("pppoe", pppoe_task)) g_task_running = false;
    return g_task_running;
}

void pppoe_stop(void) {
    g_wanted = false;
}

bool pppoe_active(void) {
    return P.state == PS_UP;
}

const char* pppoe_status(void) {
    switch (P.state) {
    case PS_IDLE: return g_wanted ? "reconnecting" : "off";
    case PS_DISCOVERY: return "looking for the ISP's access concentrator";
    case PS_REQUEST: return "opening a session";
    case PS_LCP: return "negotiating the link";
    case PS_AUTH: return "logging in";
    case PS_IPCP: return "getting an address";
    default: return "connected";
    }
}

const char* pppoe_last_error(void) {
    return P.last_error;
}

bool pppoe_wait_up(uint32_t timeout_ms) {
    uint64_t start = time_get_uptime_ms();
    while (time_get_uptime_ms() - start < timeout_ms) {
        if (P.state == PS_UP) return true;
        if (P.auth_failed) return false;
        sleep(50);
    }
    return P.state == PS_UP;
}
