#ifndef NET_SYSCALL_H
#define NET_SYSCALL_H

#include <stdint.h>
#include "x86_64/syscall.h"

/*
 * SYS_NET backing implementation. Lives in its own translation unit
 * rather than in syscall.c because it owns real state (the TCP handle
 * table and the per-port UDP receive queues), which syscall.c
 * otherwise never does.
 */

// Clears the handle table and UDP queues. Call once at boot, after
// udp_init()/tcp_init().
void net_syscall_init(void);

uint64_t sys_net_impl(syscall_net_request_t* request);

#endif // NET_SYSCALL_H
