#ifndef DHCP_H
#define DHCP_H
#include <stdint.h>
#include <stdbool.h>

// Runs DISCOVER -> OFFER -> REQUEST -> ACK, blocking (polls the NIC
// itself) for up to timeout_ms. On success configures ip.c's address.
bool dhcp_acquire(uint32_t timeout_ms);

// Same acquisition flow without diagnostic output, for background services.
bool dhcp_acquire_quiet(uint32_t timeout_ms);

#endif
