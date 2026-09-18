#ifndef DHCP_H
#define DHCP_H
#include <stdint.h>
#include <stdbool.h>

// Runs DISCOVER -> OFFER -> REQUEST -> ACK, blocking (polls the NIC
// itself) for up to timeout_ms. On success configures ip.c's address.
bool dhcp_acquire(uint32_t timeout_ms);

#endif
