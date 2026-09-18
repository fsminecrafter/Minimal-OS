#include "net/ip.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "string.h"
#include "serial.h"

static uint32_t g_local_ip = 0;
static uint32_t g_netmask = 0;
static uint32_t g_gateway = 0;
static uint32_t g_dns_ip = 0;
static uint16_t g_ip_id = 1;

void ip_init(void) {
    g_local_ip = 0;
    g_netmask = 0;
    g_gateway = 0;
    g_dns_ip = 0;
}

void ip_configure(uint32_t local_ip, uint32_t netmask, uint32_t gateway_ip) {
    g_local_ip = local_ip;
    g_netmask = netmask;
    g_gateway = gateway_ip;
}

uint32_t ip_get_local(void)   { return g_local_ip; }
uint32_t ip_get_gateway(void) { return g_gateway; }
uint32_t ip_get_netmask(void) { return g_netmask; }
void ip_set_dns(uint32_t dns_ip) { g_dns_ip = dns_ip; }
uint32_t ip_get_dns(void) { return g_dns_ip; }
bool ip_is_configured(void) { return g_local_ip != 0; }

uint32_t ip_parse(const char* s) {
    uint32_t result = 0, octet = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (uint32_t)(*s - '0');
        } else if (*s == '.') {
            result = (result << 8) | (octet & 0xFF);
            octet = 0;
        }
        s++;
    }
    result = (result << 8) | (octet & 0xFF);
    return result;
}

void ip_to_string(uint32_t ip, char* out) {
    snprintf(out, 16, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 8) & 0xFF),  (unsigned)(ip & 0xFF));
}

bool ip_send(uint32_t dst_ip, uint8_t protocol, const void* payload, uint16_t len) {
    bool broadcast = (dst_ip == 0xFFFFFFFFu);
    bool on_link = broadcast || ((dst_ip & g_netmask) == (g_local_ip & g_netmask));
    uint32_t next_hop = on_link ? dst_ip : g_gateway;

    uint8_t dst_mac[6];
    if (broadcast) {
        memset(dst_mac, 0xFF, 6);
    } else if (!arp_resolve(next_hop, dst_mac, 1000)) {
        serial_write_str("IP: ARP resolve failed\n");
        return false;
    }

    static uint8_t packet[ETH_MTU];
    uint16_t total_len = (uint16_t)(sizeof(ipv4_header_t) + len);
    if (total_len > ETH_MTU) return false;

    ipv4_header_t* hdr = (ipv4_header_t*)packet;
    hdr->ver_ihl = 0x45;
    hdr->dscp_ecn = 0;
    hdr->total_len = net_htons(total_len);
    hdr->id = net_htons(g_ip_id++);
    hdr->flags_frag = 0;
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src_ip = net_htonl(g_local_ip);
    hdr->dst_ip = net_htonl(dst_ip);
    hdr->checksum = net_htons(net_ip_checksum(hdr));

    memcpy(packet + sizeof(ipv4_header_t), payload, len);

    eth_send(dst_mac, ETHERTYPE_IPV4, packet, total_len);
    return true;
}

void ip_handle_packet(const uint8_t* frame_payload, uint16_t len) {
    if (len < sizeof(ipv4_header_t)) return;
    const ipv4_header_t* hdr = (const ipv4_header_t*)frame_payload;
    if ((hdr->ver_ihl >> 4) != 4) return;

    uint8_t ihl = (uint8_t)((hdr->ver_ihl & 0x0F) * 4);
    if (ihl < sizeof(ipv4_header_t) || len < ihl) return;

    uint32_t dst_ip = net_ntohl(hdr->dst_ip);
    if (g_local_ip != 0 && dst_ip != g_local_ip && dst_ip != 0xFFFFFFFFu) return;

    uint16_t total_len = net_ntohs(hdr->total_len);
    if (total_len > len) return;

    const uint8_t* payload = frame_payload + ihl;
    uint16_t payload_len = total_len - ihl;
    uint32_t src_ip = net_ntohl(hdr->src_ip);

    switch (hdr->protocol) {
        case IP_PROTO_UDP: udp_handle_packet(src_ip, payload, payload_len); break;
        case IP_PROTO_TCP: tcp_handle_packet(src_ip, payload, payload_len); break;
        default: break;
    }
}
