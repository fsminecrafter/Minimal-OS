#include "net/udp.h"
#include "net/ip.h"
#include "string.h"

#define UDP_MAX_HANDLERS 8

typedef struct {
    bool used;
    uint16_t port;
    udp_handler_t handler;
} udp_binding_t;

static udp_binding_t g_bindings[UDP_MAX_HANDLERS];

void udp_init(void) {
    memset(g_bindings, 0, sizeof(g_bindings));
}

bool udp_register_handler(uint16_t local_port, udp_handler_t handler) {
    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (g_bindings[i].used && g_bindings[i].port == local_port) {
            g_bindings[i].handler = handler;
            return true;
        }
    }
    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (!g_bindings[i].used) {
            g_bindings[i].used = true;
            g_bindings[i].port = local_port;
            g_bindings[i].handler = handler;
            return true;
        }
    }
    return false;
}

void udp_unregister_handler(uint16_t local_port) {
    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (g_bindings[i].used && g_bindings[i].port == local_port) g_bindings[i].used = false;
    }
}

bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void* payload, uint16_t len) {
    static uint8_t buf[ETH_MTU];
    uint16_t total = (uint16_t)(sizeof(udp_header_t) + len);
    if (total > sizeof(buf)) return false;

    udp_header_t* hdr = (udp_header_t*)buf;
    hdr->src_port = net_htons(src_port);
    hdr->dst_port = net_htons(dst_port);
    hdr->length = net_htons(total);
    hdr->checksum = 0;
    memcpy(buf + sizeof(udp_header_t), payload, len);

    hdr->checksum = net_htons(net_udp_checksum(ip_get_local(), dst_ip, hdr, total));
    if (hdr->checksum == 0) hdr->checksum = 0xFFFF;

    return ip_send(dst_ip, IP_PROTO_UDP, buf, total);
}

void udp_handle_packet(uint32_t src_ip, const uint8_t* data, uint16_t len) {
    if (len < sizeof(udp_header_t)) return;
    const udp_header_t* hdr = (const udp_header_t*)data;
    uint16_t dst_port = net_ntohs(hdr->dst_port);
    uint16_t src_port = net_ntohs(hdr->src_port);
    uint16_t udp_len = net_ntohs(hdr->length);
    if (udp_len > len) udp_len = len;
    const uint8_t* payload = data + sizeof(udp_header_t);
    uint16_t payload_len = (udp_len > sizeof(udp_header_t)) ? udp_len - sizeof(udp_header_t) : 0;

    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (g_bindings[i].used && g_bindings[i].port == dst_port && g_bindings[i].handler) {
            g_bindings[i].handler(src_ip, src_port, payload, payload_len);
            return;
        }
    }
}
