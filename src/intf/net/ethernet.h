#ifndef ETHERNET_H
#define ETHERNET_H
#include "net/net.h"

extern uint8_t g_local_mac[6];

void eth_init(void);
void eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void* payload, uint16_t len);
void eth_handle_frame(const uint8_t* frame, uint16_t len);

#endif
