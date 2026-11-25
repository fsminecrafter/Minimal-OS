#ifndef DHCP_H
#define DHCP_H
#include <stdint.h>
#include <stdbool.h>

// Runs DISCOVER -> OFFER -> REQUEST -> ACK, blocking (polls the NIC
// itself) for up to timeout_ms. On success configures ip.c's address.
bool dhcp_acquire(uint32_t timeout_ms);

// Same acquisition flow without diagnostic output, for background services.
bool dhcp_acquire_quiet(uint32_t timeout_ms);

// DHCP packets that reached port 68 during the last attempt, and how many
// of them were for another client's transaction.
void dhcp_last_stats(uint32_t* replies, uint32_t* other_xid);

// Starts the background DHCP task: it keeps trying until it gets a lease
// (backing off up to a minute between attempts) and then renews it before
// it runs out. A successful dhcp_acquire() starts it too, for renewals.
void dhcp_start_background(void);

// Stops renewing (a static address was set); the address stays as it is.
void dhcp_stop(void);

// The current lease: its length, seconds left and the server that gave it.
// False when the address did not come from DHCP.
bool dhcp_lease_info(uint32_t* lease_s, uint32_t* remaining_s, uint32_t* server);

#endif
