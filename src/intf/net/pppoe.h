#ifndef PPPOE_H
#define PPPOE_H
#include "net/net.h"

// PPPoE (DSL and many fibre lines): connects in the background with the
// ISP's user name and password, keeps the session alive and reconnects
// when it drops. While connected, IPv4 goes through the session.
bool pppoe_start(const char* user, const char* password);
void pppoe_stop(void);
bool pppoe_active(void);
// Waits until connected; false on timeout or a rejected login.
bool pppoe_wait_up(uint32_t timeout_ms);
const char* pppoe_status(void);
const char* pppoe_last_error(void);   // "" when there was none

// For ethernet.c / ip.c
void pppoe_handle_frame(const uint8_t src_mac[6], uint16_t ethertype,
                        const uint8_t* payload, uint16_t len);
bool pppoe_send_ip(const void* packet, uint16_t len);

#endif
