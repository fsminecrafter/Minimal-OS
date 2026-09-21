#include "net/dhcp.h"
#include "net/udp.h"
#include "net/ip.h"
#include "net/ethernet.h"
#include "x86_64/network_manager.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "string.h"
#include "serial.h"
#include <stddef.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC_COOKIE 0x63825363u
#define DHCP_OP_REQUEST 1
#define DHCP_HTYPE_ETH  1

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
    uint8_t  options[312];
} dhcp_packet_t;

static volatile bool g_offer_received = false;
static volatile bool g_ack_received = false;
static volatile bool g_nak_received = false;
static uint32_t g_offered_ip;
static uint32_t g_server_ip;
static uint32_t g_offered_netmask = 0xFFFFFF00u;
static uint32_t g_offered_gateway;
static uint32_t g_offered_dns;
static uint32_t g_xid;

static uint8_t* dhcp_put_option(uint8_t* p, uint8_t code, uint8_t len, const void* data) {
    *p++ = code;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

static void dhcp_send(uint8_t msg_type, uint32_t requested_ip, uint32_t server_ip_for_request) {
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = DHCP_OP_REQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = 6;
    pkt.xid = net_htonl(g_xid);
    memcpy(pkt.chaddr, g_local_mac, 6);
    pkt.magic_cookie = net_htonl(DHCP_MAGIC_COOKIE);

    uint8_t* p = pkt.options;
    p = dhcp_put_option(p, 53, 1, &msg_type);
    if (msg_type == DHCPREQUEST) {
        uint32_t req_ip_be = net_htonl(requested_ip);
        p = dhcp_put_option(p, 50, 4, &req_ip_be);
        uint32_t server_be = net_htonl(server_ip_for_request);
        p = dhcp_put_option(p, 54, 4, &server_be);
    }
    uint8_t param_list[] = {1, 3, 6};
    p = dhcp_put_option(p, 55, sizeof(param_list), param_list);
    *p++ = 0xFF;

    uint16_t opt_len = (uint16_t)(p - pkt.options);
    uint16_t total_len = (uint16_t)(offsetof(dhcp_packet_t, options) + opt_len);

    udp_send(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &pkt, total_len);
}

static const uint8_t* dhcp_find_option(const dhcp_packet_t* pkt, uint16_t opt_area_len,
                                       uint8_t code, uint8_t* out_len) {
    const uint8_t* p = pkt->options;
    const uint8_t* end = pkt->options + opt_area_len;
    while (p < end && *p != 0xFF) {
        if (*p == 0) { p++; continue; }
        uint8_t c = *p++;
        if (p >= end) break;
        uint8_t len = *p++;
        if (p + len > end) break;
        if (c == code) { if (out_len) *out_len = len; return p; }
        p += len;
    }
    return NULL;
}

static void dhcp_handle_reply(uint32_t src_ip, uint16_t src_port,
                              const uint8_t* data, uint16_t len) {
    (void)src_ip; (void)src_port;
    if (len < offsetof(dhcp_packet_t, options)) return;
    const dhcp_packet_t* pkt = (const dhcp_packet_t*)data;
    if (net_ntohl(pkt->xid) != g_xid) return;
    if (net_ntohl(pkt->magic_cookie) != DHCP_MAGIC_COOKIE) return;

    uint16_t opt_len = (uint16_t)(len - offsetof(dhcp_packet_t, options));
    uint8_t type_len = 0;
    const uint8_t* type = dhcp_find_option(pkt, opt_len, 53, &type_len);
    if (!type || type_len < 1) return;

    if (*type == DHCPOFFER) {
        g_offered_ip = net_ntohl(pkt->yiaddr);
        uint8_t l;
        const uint8_t* server_id = dhcp_find_option(pkt, opt_len, 54, &l);
        g_server_ip = (server_id && l == 4) ? net_ntohl(*(const uint32_t*)server_id) : net_ntohl(pkt->siaddr);
        const uint8_t* mask = dhcp_find_option(pkt, opt_len, 1, &l);
        if (mask && l == 4) g_offered_netmask = net_ntohl(*(const uint32_t*)mask);
        const uint8_t* router = dhcp_find_option(pkt, opt_len, 3, &l);
        if (router && l >= 4) g_offered_gateway = net_ntohl(*(const uint32_t*)router);
        const uint8_t* dns = dhcp_find_option(pkt, opt_len, 6, &l);
        if (dns && l >= 4) g_offered_dns = net_ntohl(*(const uint32_t*)dns);
        g_offer_received = true;
    } else if (*type == DHCPACK) {
        g_ack_received = true;
    } else if (*type == DHCPNAK) {
        g_nak_received = true;
    }
}

static bool dhcp_acquire_internal(uint32_t timeout_ms, bool quiet) {
    g_offer_received = g_ack_received = g_nak_received = false;
    g_xid = (uint32_t)time_get_uptime_ms() ^ 0xA5A5A5A5u;

    udp_register_handler(DHCP_CLIENT_PORT, dhcp_handle_reply);

    if (!quiet) serial_write_str("DHCP: sending DISCOVER\n");
    dhcp_send(DHCPDISCOVER, 0, 0);

    uint64_t start = time_get_uptime_ms();
    while (!g_offer_received && time_get_uptime_ms() - start < timeout_ms) {
        network_manager_poll();
        sleep(10);
    }
    if (!g_offer_received) {
        if (!quiet) serial_write_str("DHCP: no OFFER received\n");
        udp_unregister_handler(DHCP_CLIENT_PORT);
        return false;
    }

    if (!quiet) serial_write_str("DHCP: got OFFER, sending REQUEST\n");
    dhcp_send(DHCPREQUEST, g_offered_ip, g_server_ip);

    start = time_get_uptime_ms();
    while (!g_ack_received && !g_nak_received && time_get_uptime_ms() - start < timeout_ms) {
        network_manager_poll();
        sleep(10);
    }
    udp_unregister_handler(DHCP_CLIENT_PORT);

    if (g_nak_received || !g_ack_received) {
        if (!quiet) serial_write_str("DHCP: request denied or timed out\n");
        return false;
    }

    ip_configure(g_offered_ip, g_offered_netmask, g_offered_gateway);
    ip_set_dns(g_offered_dns);

    char buf[16];
    ip_to_string(g_offered_ip, buf);
    if (!quiet) {
        serial_write_str("DHCP: bound "); serial_write_str(buf); serial_write_str("\n");
    }
    return true;
}

bool dhcp_acquire(uint32_t timeout_ms) {
    return dhcp_acquire_internal(timeout_ms, false);
}

bool dhcp_acquire_quiet(uint32_t timeout_ms) {
    return dhcp_acquire_internal(timeout_ms, true);
}
