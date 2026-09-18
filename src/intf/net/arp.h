#ifndef ARP_H
#define ARP_H
#include "net/net.h"

void arp_init(void);
void arp_handle_packet(const arp_packet_t* pkt);
void arp_send_request(uint32_t target_ip);
bool arp_resolve(uint32_t target_ip, uint8_t out_mac[6], uint32_t timeout_ms);
void arp_cache_insert(uint32_t ip, const uint8_t mac[6]);

#endif
