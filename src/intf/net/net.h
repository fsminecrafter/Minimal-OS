#ifndef NET_H
#define NET_H
#include <stdint.h>
#include <stdbool.h>

static inline uint16_t net_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t net_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
#define net_ntohs net_htons
#define net_ntohl net_htonl

#define ETH_ADDR_LEN 6
#define ETH_MTU 1500
#define ETH_FRAME_MAX (14 + ETH_MTU)

typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} eth_header_t;

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
} arp_packet_t;

#define ARP_HTYPE_ETHERNET 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_header_t;

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset; // high 4 bits = header length in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

uint32_t net_checksum_add(const void* data, uint32_t len, uint32_t sum);
uint16_t net_checksum_fold(uint32_t sum);
uint16_t net_ip_checksum(const ipv4_header_t* hdr);
uint16_t net_udp_checksum(uint32_t src_ip, uint32_t dst_ip, const udp_header_t* udp, uint16_t udp_len);
uint16_t net_tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void* tcp_seg, uint16_t seg_len);

#endif
