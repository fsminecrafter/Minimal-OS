#ifndef IP_H
#define IP_H
#include "net/net.h"

void ip_init(void);
void ip_configure(uint32_t local_ip, uint32_t netmask, uint32_t gateway_ip);
uint32_t ip_get_local(void);
uint32_t ip_get_gateway(void);
uint32_t ip_get_netmask(void);
void ip_set_dns(uint32_t dns_ip);
uint32_t ip_get_dns(void);
bool ip_is_configured(void);

bool ip_send(uint32_t dst_ip, uint8_t protocol, const void* payload, uint16_t len);
void ip_handle_packet(const uint8_t* frame_payload, uint16_t len);

uint32_t ip_parse(const char* dotted);
void ip_to_string(uint32_t ip, char* out /* >= 16 bytes */);

#endif
