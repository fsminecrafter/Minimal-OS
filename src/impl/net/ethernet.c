#include "net/ethernet.h"
#include "net/arp.h"
#include "net/ip.h"
#include "x86_64/network_manager.h"
#include "string.h"
#include "serial.h"

uint8_t g_local_mac[6] = {0};

void eth_init(void) {
    network_manager_get_mac(g_local_mac);
    serial_write_str("ETH: local MAC = ");
    for (int i = 0; i < 6; i++) {
        serial_write_hex(g_local_mac[i]);
        if (i < 5) serial_write_str(":");
    }
    serial_write_str("\n");
}

void eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void* payload, uint16_t len) {
    static uint8_t frame[ETH_FRAME_MAX];
    if (len > ETH_MTU) len = ETH_MTU;

    eth_header_t* hdr = (eth_header_t*)frame;
    memcpy(hdr->dst_mac, dst_mac, 6);
    memcpy(hdr->src_mac, g_local_mac, 6);
    hdr->ethertype = net_htons(ethertype);

    memcpy(frame + sizeof(eth_header_t), payload, len);
    network_manager_send(frame, (uint16_t)(sizeof(eth_header_t) + len));
}

/*
 * Learn the sender's MAC from an inbound IPv4 frame, for on-link
 * senders only.
 *
 * The reply to a SYN, a request or a data segment goes straight back
 * to whoever sent it, and ip_send() has to know that host's MAC to do
 * so. Normally the ARP exchange that preceded the packet already put it
 * in the cache - but the cache is 16 entries with crude eviction, and
 * when it misses, arp_resolve() blocks for up to a second calling
 * network_manager_poll() from INSIDE the receive handler that is
 * already running inside network_manager_poll(). The NIC driver's
 * linearisation buffer is static, so that nested poll overwrites the
 * frame the outer handler is still reading.
 *
 * Learning here makes "answer the host that just spoke to us" a cache
 * hit by construction. Off-link senders are skipped: their frames carry
 * the gateway's MAC, not theirs.
 */
static void eth_learn_sender(const uint8_t src_mac[6], const uint8_t* ip_pkt, uint16_t len) {
    if (len < sizeof(ipv4_header_t)) return;
    if (!ip_is_configured()) return;

    const ipv4_header_t* iph = (const ipv4_header_t*)ip_pkt;
    uint32_t src_ip = net_ntohl(iph->src_ip);
    uint32_t local = ip_get_local();
    uint32_t mask = ip_get_netmask();

    if (src_ip == 0 || src_ip == local || src_ip == 0xFFFFFFFFu) return;
    if (mask == 0 || (src_ip & mask) != (local & mask)) return;

    arp_cache_insert(src_ip, src_mac);
}

void eth_handle_frame(const uint8_t* frame, uint16_t len) {
    if (len < sizeof(eth_header_t)) return;
    const eth_header_t* hdr = (const eth_header_t*)frame;
    uint16_t ethertype = net_ntohs(hdr->ethertype);
    const uint8_t* payload = frame + sizeof(eth_header_t);
    uint16_t payload_len = len - sizeof(eth_header_t);

    switch (ethertype) {
        case ETHERTYPE_ARP:
            if (payload_len >= sizeof(arp_packet_t)) {
                arp_handle_packet((const arp_packet_t*)payload);
            }
            break;
        case ETHERTYPE_IPV4:
            eth_learn_sender(hdr->src_mac, payload, payload_len);
            ip_handle_packet(payload, payload_len);
            break;
        default:
            break;
    }
}
