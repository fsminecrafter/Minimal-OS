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
            ip_handle_packet(payload, payload_len);
            break;
        default:
            break;
    }
}
