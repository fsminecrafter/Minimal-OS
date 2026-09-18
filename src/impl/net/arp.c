#include "net/arp.h"
#include "net/ethernet.h"
#include "net/ip.h"
#include "x86_64/network_manager.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "string.h"
#include "serial.h"

#define ARP_CACHE_SIZE 16

typedef struct {
    bool used;
    uint32_t ip;
    uint8_t mac[6];
} arp_entry_t;

static arp_entry_t g_cache[ARP_CACHE_SIZE];
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void arp_init(void) {
    memset(g_cache, 0, sizeof(g_cache));
}

void arp_cache_insert(uint32_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && g_cache[i].ip == ip) {
            memcpy(g_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].used) {
            g_cache[i].used = true;
            g_cache[i].ip = ip;
            memcpy(g_cache[i].mac, mac, 6);
            return;
        }
    }
    g_cache[0].used = true;      // cache full - simple eviction
    g_cache[0].ip = ip;
    memcpy(g_cache[0].mac, mac, 6);
}

static bool arp_cache_lookup(uint32_t ip, uint8_t out_mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && g_cache[i].ip == ip) {
            memcpy(out_mac, g_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

void arp_send_request(uint32_t target_ip) {
    arp_packet_t pkt;
    pkt.htype = net_htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = net_htons(ETHERTYPE_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = net_htons(ARP_OP_REQUEST);
    memcpy(pkt.sender_mac, g_local_mac, 6);
    pkt.sender_ip = net_htonl(ip_get_local());
    memset(pkt.target_mac, 0, 6);
    pkt.target_ip = net_htonl(target_ip);

    eth_send(BROADCAST_MAC, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

void arp_handle_packet(const arp_packet_t* pkt) {
    uint16_t oper = net_ntohs(pkt->oper);
    uint32_t sender_ip = net_ntohl(pkt->sender_ip);

    arp_cache_insert(sender_ip, pkt->sender_mac);

    if (oper == ARP_OP_REQUEST && net_ntohl(pkt->target_ip) == ip_get_local()) {
        arp_packet_t reply;
        reply.htype = net_htons(ARP_HTYPE_ETHERNET);
        reply.ptype = net_htons(ETHERTYPE_IPV4);
        reply.hlen = 6;
        reply.plen = 4;
        reply.oper = net_htons(ARP_OP_REPLY);
        memcpy(reply.sender_mac, g_local_mac, 6);
        reply.sender_ip = net_htonl(ip_get_local());
        memcpy(reply.target_mac, pkt->sender_mac, 6);
        reply.target_ip = pkt->sender_ip;

        eth_send(pkt->sender_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
    }
}

bool arp_resolve(uint32_t target_ip, uint8_t out_mac[6], uint32_t timeout_ms) {
    if (arp_cache_lookup(target_ip, out_mac)) return true;

    arp_send_request(target_ip);

    uint64_t start = time_get_uptime_ms();
    while (time_get_uptime_ms() - start < timeout_ms) {
        network_manager_poll();
        if (arp_cache_lookup(target_ip, out_mac)) return true;
        sleep(2);
    }
    return false;
}
