#ifndef NET_SYSCALL_H
#define NET_SYSCALL_H

#include <stdbool.h>
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

// For kernel code that drives the stack itself (the stack is not
// reentrant): waits briefly for whoever is inside it and takes it.
// Returns false if it stayed busy. Hold it for short slices only.
bool net_stack_claim(void);
void net_stack_release(void);

#endif // NET_SYSCALL_H
