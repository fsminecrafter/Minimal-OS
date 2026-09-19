#include "x86_64/syscall.h"
#include "x86_64/network_manager.h"
#include "x86_64/spinlock.h"
#include "x86_64/random.h"
#include "net/net.h"
#include "net/ip.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "net/dns.h"
#include "net/dhcp.h"
#include "net/ethernet.h"
#include "string.h"
#include "serial.h"
#include "time.h"

/*
 * SYS_NET - userland access to the kernel network stack.
 *
 * Three things this file exists to provide that the raw stack does not:
 *
 * 1. Handles instead of pointers. tcp_connect() hands back a
 *    tcp_conn_t*, which is a kernel struct with a hardcoded inline
 *    receive buffer. Userland gets a small integer instead, so the
 *    kernel can validate it and so tcp_conn_t stays free to change.
 *
 * 2. UDP receive queues. udp_register_handler() takes a KERNEL
 *    CALLBACK invoked from the NIC poll path. A user process cannot be
 *    called back like that, so each bound port gets a queue here that
 *    the callback fills and SYS_NET_UDP_RECV drains. This is what
 *    makes Deliver's UDP:4243 LAN discovery reachable from a .run
 *    program.
 *
 * 3. Serialization. ip_send(), udp_send() and tcp_send_segment() all
 *    build their frames in FILE-STATIC buffers, so the stack is not
 *    reentrant, and with SMP up two cores can genuinely be inside it
 *    at once. Rather than spin (these operations block for whole
 *    seconds - tcp_connect() retries for up to 3s), we try-lock and
 *    return SYS_ERR_BUSY so the caller can retry or back off. Making
 *    the stack properly reentrant is the real fix and is deliberately
 *    deferred - see the note at the bottom of this file.
 */

#define NET_MAX_TCP_HANDLES  4      // matches TCP_MAX_CONNS in tcp.c
#define NET_MAX_UDP_BINDINGS 4      // udp.c allows 8; 4 trampolines is plenty
#define NET_UDP_QUEUE_DEPTH  8
#define NET_UDP_DGRAM_MAX    1024   // discovery + DNS sized; larger is dropped

// ---------------------------------------------------------------------------
// Non-reentrancy guard
// ---------------------------------------------------------------------------

static volatile uint32_t g_net_busy = 0;

static bool net_try_claim(void) {
    return __sync_bool_compare_and_swap(&g_net_busy, 0, 1);
}

static void net_release(void) {
    __sync_lock_release(&g_net_busy);
}

// ---------------------------------------------------------------------------
// TCP handle table
// ---------------------------------------------------------------------------

typedef struct {
    bool in_use;
    tcp_conn_t* conn;
} net_tcp_slot_t;

static net_tcp_slot_t g_tcp_slots[NET_MAX_TCP_HANDLES];

// Handles are 1-based so 0 is always invalid.
static uint64_t tcp_slot_alloc(tcp_conn_t* conn) {
    for (int i = 0; i < NET_MAX_TCP_HANDLES; i++) {
        if (!g_tcp_slots[i].in_use) {
            g_tcp_slots[i].in_use = true;
            g_tcp_slots[i].conn = conn;
            return (uint64_t)(i + 1);
        }
    }
    return 0;
}

static net_tcp_slot_t* tcp_slot_get(uint64_t handle) {
    if (handle == 0 || handle > NET_MAX_TCP_HANDLES) return NULL;
    net_tcp_slot_t* slot = &g_tcp_slots[handle - 1];
    if (!slot->in_use || !slot->conn) return NULL;
    return slot;
}

static void tcp_slot_free(uint64_t handle) {
    net_tcp_slot_t* slot = tcp_slot_get(handle);
    if (!slot) return;
    slot->in_use = false;
    slot->conn = NULL;
}

// ---------------------------------------------------------------------------
// UDP receive queues
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[NET_UDP_DGRAM_MAX];
} net_udp_dgram_t;

typedef struct {
    bool     in_use;
    uint16_t port;
    uint32_t head;      // next slot to write
    uint32_t tail;      // next slot to read
    uint32_t count;
    uint32_t dropped;   // queue-full drops since bind, for diagnostics
    net_udp_dgram_t ring[NET_UDP_QUEUE_DEPTH];
} net_udp_binding_t;

static net_udp_binding_t g_udp_bindings[NET_MAX_UDP_BINDINGS];
static spinlock_t g_udp_lock = SPINLOCK_INIT;

// Called from the NIC poll path (udp_handle_packet -> handler), which
// may be a different context from the process that will drain it, so
// the ring is spinlock-protected even though the stack itself is
// currently single-threaded.
static void udp_enqueue(int index, uint32_t src_ip, uint16_t src_port,
                        const uint8_t* data, uint16_t len) {
    if (index < 0 || index >= NET_MAX_UDP_BINDINGS) return;
    if (len > NET_UDP_DGRAM_MAX) len = NET_UDP_DGRAM_MAX;

    uint64_t flags = spinlock_acquire(&g_udp_lock);
    net_udp_binding_t* b = &g_udp_bindings[index];
    if (!b->in_use) { spinlock_release(&g_udp_lock, flags); return; }

    if (b->count == NET_UDP_QUEUE_DEPTH) {
        // Drop the OLDEST, not the newest: for discovery traffic the
        // most recent server announcement is the interesting one.
        b->tail = (b->tail + 1) % NET_UDP_QUEUE_DEPTH;
        b->count--;
        b->dropped++;
    }

    net_udp_dgram_t* d = &b->ring[b->head];
    d->src_ip = src_ip;
    d->src_port = src_port;
    d->len = len;
    if (len) memcpy(d->data, data, len);

    b->head = (b->head + 1) % NET_UDP_QUEUE_DEPTH;
    b->count++;
    spinlock_release(&g_udp_lock, flags);

    // Packet arrival timing is a cheap entropy source and this is the
    // one place in the kernel that sees it.
    random_add_entropy(((uint64_t)src_ip << 16) ^ src_port ^ time_get_uptime_ms());
}

// udp_handler_t carries no cookie, so one trampoline per binding slot.
static void udp_cb0(uint32_t ip, uint16_t port, const uint8_t* d, uint16_t l) { udp_enqueue(0, ip, port, d, l); }
static void udp_cb1(uint32_t ip, uint16_t port, const uint8_t* d, uint16_t l) { udp_enqueue(1, ip, port, d, l); }
static void udp_cb2(uint32_t ip, uint16_t port, const uint8_t* d, uint16_t l) { udp_enqueue(2, ip, port, d, l); }
static void udp_cb3(uint32_t ip, uint16_t port, const uint8_t* d, uint16_t l) { udp_enqueue(3, ip, port, d, l); }

static const udp_handler_t g_udp_trampolines[NET_MAX_UDP_BINDINGS] = {
    udp_cb0, udp_cb1, udp_cb2, udp_cb3
};

static uint64_t udp_bind_port(uint16_t port) {
    for (int i = 0; i < NET_MAX_UDP_BINDINGS; i++) {
        if (g_udp_bindings[i].in_use && g_udp_bindings[i].port == port) {
            return SYS_ERR_INVAL;   // already bound; no SO_REUSEPORT here
        }
    }
    for (int i = 0; i < NET_MAX_UDP_BINDINGS; i++) {
        if (g_udp_bindings[i].in_use) continue;

        uint64_t flags = spinlock_acquire(&g_udp_lock);
        memset(&g_udp_bindings[i], 0, sizeof(g_udp_bindings[i]));
        g_udp_bindings[i].in_use = true;
        g_udp_bindings[i].port = port;
        spinlock_release(&g_udp_lock, flags);

        if (!udp_register_handler(port, g_udp_trampolines[i])) {
            g_udp_bindings[i].in_use = false;
            return SYS_ERR_GENERIC;
        }
        return (uint64_t)(i + 1);
    }
    return SYS_ERR_GENERIC;
}

static net_udp_binding_t* udp_binding_get(uint64_t handle) {
    if (handle == 0 || handle > NET_MAX_UDP_BINDINGS) return NULL;
    net_udp_binding_t* b = &g_udp_bindings[handle - 1];
    return b->in_use ? b : NULL;
}

static uint64_t udp_unbind(uint64_t handle) {
    net_udp_binding_t* b = udp_binding_get(handle);
    if (!b) return SYS_ERR_BADFD;
    udp_unregister_handler(b->port);
    uint64_t flags = spinlock_acquire(&g_udp_lock);
    b->in_use = false;
    spinlock_release(&g_udp_lock, flags);
    return SYS_SUCCESS;
}

static uint64_t udp_dequeue(net_udp_binding_t* b, void* buf, uint32_t maxlen,
                            uint32_t* out_ip, uint16_t* out_port) {
    uint64_t flags = spinlock_acquire(&g_udp_lock);
    if (b->count == 0) {
        spinlock_release(&g_udp_lock, flags);
        return 0;
    }

    net_udp_dgram_t* d = &b->ring[b->tail];
    uint32_t copy = d->len;
    if (copy > maxlen) copy = maxlen;   // truncated; remainder is discarded
    if (copy) memcpy(buf, d->data, copy);
    if (out_ip) *out_ip = d->src_ip;
    if (out_port) *out_port = d->src_port;

    b->tail = (b->tail + 1) % NET_UDP_QUEUE_DEPTH;
    b->count--;
    spinlock_release(&g_udp_lock, flags);
    return copy;
}

// ---------------------------------------------------------------------------
// State mapping
// ---------------------------------------------------------------------------

static uint64_t map_tcp_state(const tcp_conn_t* conn) {
    if (!conn) return SYSCALL_NET_TCP_CLOSED;
    switch (conn->state) {
        case TCP_SYN_SENT:    return SYSCALL_NET_TCP_CONNECTING;
        case TCP_ESTABLISHED: return SYSCALL_NET_TCP_ESTABLISHED;
        case TCP_FIN_WAIT:    return SYSCALL_NET_TCP_CLOSING;
        case TCP_CLOSE_WAIT:  return SYSCALL_NET_TCP_PEER_CLOSED;
        case TCP_CLOSED:
        default:              return SYSCALL_NET_TCP_CLOSED;
    }
}

// ---------------------------------------------------------------------------
// Entry point (called from syscall_dispatch)
// ---------------------------------------------------------------------------

void net_syscall_init(void) {
    memset(g_tcp_slots, 0, sizeof(g_tcp_slots));
    memset(g_udp_bindings, 0, sizeof(g_udp_bindings));
    g_net_busy = 0;
}

uint64_t sys_net_impl(syscall_net_request_t* request) {
    if (!request) return SYS_ERR_INVAL;

    // Ops that touch neither the send path nor the handle tables can
    // run without claiming the stack.
    switch (request->op) {
        case SYS_NET_STATUS: {
            if (!request->out_status) return SYS_ERR_INVAL;
            syscall_net_status_t* out = request->out_status;
            memset(out, 0, sizeof(*out));
            out->has_driver = network_manager_has_driver() ? 1 : 0;
            if (out->has_driver) network_manager_get_mac(out->mac);
            out->configured = ip_is_configured() ? 1 : 0;
            out->local_ip = ip_get_local();
            out->netmask  = ip_get_netmask();
            out->gateway  = ip_get_gateway();
            out->dns      = ip_get_dns();
            return SYS_SUCCESS;
        }

        case SYS_NET_TCP_STATE: {
            net_tcp_slot_t* slot = tcp_slot_get(request->handle);
            if (!slot) return SYS_ERR_BADFD;
            return map_tcp_state(slot->conn);
        }

        default:
            break;
    }

    if (!network_manager_has_driver()) return SYS_ERR_GENERIC;

    if (!net_try_claim()) return SYS_ERR_BUSY;
    uint64_t result;

    switch (request->op) {
        case SYS_NET_POLL:
            network_manager_poll();
            result = SYS_SUCCESS;
            break;

        case SYS_NET_DHCP:
            // Privilege-gated in syscall_user_may_call() - this
            // rewrites the interface's address for every process.
            result = dhcp_acquire(request->timeout_ms ? request->timeout_ms : 10000)
                     ? SYS_SUCCESS : SYS_ERR_GENERIC;
            break;

        case SYS_NET_RESOLVE: {
            if (!request->host || !request->out_ip) { result = SYS_ERR_INVAL; break; }
            if (!ip_is_configured()) { result = SYS_ERR_GENERIC; break; }
            uint32_t ip = 0;
            bool ok = dns_resolve(request->host, &ip,
                                  request->timeout_ms ? request->timeout_ms : 5000);
            if (ok) *request->out_ip = ip;
            result = ok ? SYS_SUCCESS : SYS_ERR_NOTFOUND;
            break;
        }

        case SYS_NET_TCP_CONNECT: {
            if (!request->out_handle || request->ip == 0 || request->port == 0) {
                result = SYS_ERR_INVAL; break;
            }
            if (!ip_is_configured()) { result = SYS_ERR_GENERIC; break; }

            tcp_conn_t* conn = tcp_connect(request->ip, request->port,
                                           request->timeout_ms ? request->timeout_ms : 5000);
            if (!conn) { result = SYS_ERR_GENERIC; break; }

            uint64_t handle = tcp_slot_alloc(conn);
            if (handle == 0) {
                // No free handle slot even though the stack gave us a
                // connection - close it rather than leaking it.
                tcp_close(conn);
                result = SYS_ERR_GENERIC;
                break;
            }
            *request->out_handle = handle;
            result = SYS_SUCCESS;
            break;
        }

        case SYS_NET_TCP_SEND: {
            net_tcp_slot_t* slot = tcp_slot_get(request->handle);
            if (!slot) { result = SYS_ERR_BADFD; break; }
            if (!request->buf || request->len == 0) { result = SYS_ERR_INVAL; break; }

            int32_t sent = tcp_send(slot->conn, request->buf, request->len,
                                    request->timeout_ms);
            // Partial sends are real (tcp_send gives up after
            // TCP_MAX_RETRIES on a chunk) - report the count and let
            // userland decide.
            result = (sent < 0) ? SYS_ERR_GENERIC : (uint64_t)sent;
            break;
        }

        case SYS_NET_TCP_RECV: {
            net_tcp_slot_t* slot = tcp_slot_get(request->handle);
            if (!slot) { result = SYS_ERR_BADFD; break; }
            if (!request->buf || request->len == 0) { result = SYS_ERR_INVAL; break; }

            int32_t got = tcp_recv(slot->conn, request->buf, request->len);
            if (got < 0) {
                // Peer closed and the buffer is drained. Distinct from
                // 0 ("nothing yet, try again").
                result = SYS_ERR_NOTFOUND;
                break;
            }
            result = (uint64_t)got;
            break;
        }

        case SYS_NET_TCP_CLOSE: {
            net_tcp_slot_t* slot = tcp_slot_get(request->handle);
            if (!slot) { result = SYS_ERR_BADFD; break; }
            tcp_close(slot->conn);
            tcp_slot_free(request->handle);
            result = SYS_SUCCESS;
            break;
        }

        case SYS_NET_UDP_BIND:
            if (request->local_port == 0 || !request->out_handle) { result = SYS_ERR_INVAL; break; }
            result = udp_bind_port(request->local_port);
            if (result >= 1 && result <= NET_MAX_UDP_BINDINGS) {
                *request->out_handle = result;
                result = SYS_SUCCESS;
            }
            break;

        case SYS_NET_UDP_UNBIND:
            result = udp_unbind(request->handle);
            break;

        case SYS_NET_UDP_RECV: {
            net_udp_binding_t* b = udp_binding_get(request->handle);
            if (!b) { result = SYS_ERR_BADFD; break; }
            if (!request->buf || request->len == 0) { result = SYS_ERR_INVAL; break; }

            // Pump the NIC first so a caller polling in a tight loop
            // actually makes progress without a separate POLL call.
            network_manager_poll();
            result = udp_dequeue(b, request->buf, request->len,
                                 request->out_from_ip, request->out_from_port);
            break;
        }

        case SYS_NET_UDP_SEND: {
            if (!request->buf || request->len == 0) { result = SYS_ERR_INVAL; break; }
            if (request->len > NET_UDP_DGRAM_MAX) { result = SYS_ERR_INVAL; break; }
            if (!ip_is_configured()) { result = SYS_ERR_GENERIC; break; }

            uint16_t src_port = request->local_port;
            if (src_port == 0) {
                net_udp_binding_t* b = udp_binding_get(request->handle);
                if (b) src_port = b->port;
            }
            if (src_port == 0) { result = SYS_ERR_INVAL; break; }

            bool ok = udp_send(request->ip, src_port, request->port,
                               request->buf, (uint16_t)request->len);
            result = ok ? (uint64_t)request->len : SYS_ERR_GENERIC;
            break;
        }

        default:
            result = SYS_ERR_INVAL;
            break;
    }

    net_release();
    return result;
}

/*
 * Deferred, deliberately:
 *
 * - Reentrancy. The BUSY return is a guard, not a solution. Making
 *   ip_send()/udp_send()/tcp_send_segment() use per-call buffers (or a
 *   small pool) would let two processes use the network at once. Worth
 *   doing before anything long-running (a download) shares the box
 *   with the terminal's own net commands.
 *
 * - Inbound connections. tcp_handle_packet() drops any segment with no
 *   matching tcp_find() entry, so there is no LISTEN/accept path and
 *   nothing here exposes one. That is the blocker for running a
 *   Deliver SERVER on Minimal-OS and is the next milestone after the
 *   client works.
 *
 * - Per-process ownership. Handles are global, not per-process: if a
 *   program exits without closing, its connection leaks until reboot.
 *   Hooking teardown into process_exit() (alongside cleanup_entry)
 *   is the fix.
 */
