#ifndef ETHERNET_H
#define ETHERNET_H
#include "net/net.h"

extern uint8_t g_local_mac[6];

void eth_init(void);
void eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void* payload, uint16_t len);
void eth_handle_frame(const uint8_t* frame, uint16_t len);
// Tags every frame with this 802.1Q VLAN (and only accepts frames tagged
// with it); 0 turns tagging off.
void eth_set_vlan(uint16_t vlan_id);
uint16_t eth_get_vlan(void);

#endif
