#include "net/net.h"

uint32_t net_checksum_add(const void* data, uint32_t len, uint32_t sum) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t net_checksum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// Caller must zero hdr->checksum before calling this.
uint16_t net_ip_checksum(const ipv4_header_t* hdr) {
    uint32_t sum = net_checksum_add(hdr, (uint32_t)(hdr->ver_ihl & 0x0F) * 4, 0);
    return net_checksum_fold(sum);
}

static uint32_t pseudo_header_sum(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol, uint16_t len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += protocol;
    sum += len;
    return sum;
}

// Caller must zero the checksum field before calling these.
uint16_t net_udp_checksum(uint32_t src_ip, uint32_t dst_ip, const udp_header_t* udp, uint16_t udp_len) {
    uint32_t sum = pseudo_header_sum(src_ip, dst_ip, IP_PROTO_UDP, udp_len);
    sum = net_checksum_add(udp, udp_len, sum);
    return net_checksum_fold(sum);
}

uint16_t net_tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void* tcp_seg, uint16_t seg_len) {
    uint32_t sum = pseudo_header_sum(src_ip, dst_ip, IP_PROTO_TCP, seg_len);
    sum = net_checksum_add(tcp_seg, seg_len, sum);
    return net_checksum_fold(sum);
}
