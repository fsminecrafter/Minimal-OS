#include "net/ethernet.h"
#include "net/arp.h"
#include "net/ip.h"
#include "net/pppoe.h"
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

/* 802.1Q VLAN that every frame is tagged with (some ISPs require one), 0: none */
static uint16_t g_vlan;

void eth_set_vlan(uint16_t vlan_id) { g_vlan = vlan_id & 0x0FFF; }
uint16_t eth_get_vlan(void) { return g_vlan; }

void eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void* payload, uint16_t len) {
    static uint8_t frame[ETH_FRAME_MAX + 4];
    if (len > ETH_MTU) len = ETH_MTU;

    eth_header_t* hdr = (eth_header_t*)frame;
    memcpy(hdr->dst_mac, dst_mac, 6);
    memcpy(hdr->src_mac, g_local_mac, 6);
    uint16_t header = sizeof(eth_header_t);
    if (g_vlan) {
        hdr->ethertype = net_htons(0x8100);
        frame[14] = (uint8_t)(g_vlan >> 8);
        frame[15] = (uint8_t)g_vlan;
        frame[16] = (uint8_t)(ethertype >> 8);
        frame[17] = (uint8_t)ethertype;
        header += 4;
    } else {
        hdr->ethertype = net_htons(ethertype);
    }

    memcpy(frame + header, payload, len);
    network_manager_send(frame, (uint16_t)(header + len));
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
    if (ethertype == 0x8100) {
        /* VLAN-tagged: only our VLAN */
        if (payload_len < 4 || ((payload[0] << 8 | payload[1]) & 0x0FFF) != g_vlan) return;
        ethertype = (uint16_t)(payload[2] << 8 | payload[3]);
        payload += 4;
        payload_len -= 4;
    } else if (g_vlan) {
        return;   /* untagged traffic is not ours while a VLAN is set */
    }

    switch (ethertype) {
        case 0x8863:
        case 0x8864:
            pppoe_handle_frame(hdr->src_mac, ethertype, payload, payload_len);
            break;
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
