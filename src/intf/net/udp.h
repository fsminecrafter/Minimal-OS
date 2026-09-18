#ifndef UDP_H
#define UDP_H
#include "net/net.h"

typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port,
                              const uint8_t* data, uint16_t len);

void udp_init(void);
bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void* payload, uint16_t len);
bool udp_register_handler(uint16_t local_port, udp_handler_t handler);
void udp_unregister_handler(uint16_t local_port);
void udp_handle_packet(uint32_t src_ip, const uint8_t* data, uint16_t len);

#endif
