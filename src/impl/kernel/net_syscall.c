#include "x86_64/syscall.h"
#include "x86_64/network_manager.h"
#include "x86_64/spinlock.h"
#include "x86_64/random.h"
#include "x86_64/proc.h"
#include "x86_64/allocator.h"
#include "x86_64/scheduler.h"
#include "prochandler.h"
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
 *    at once. The claim waits a short, bounded time for the current
 *    holder (see net_claim_wait) and then returns SYS_ERR_BUSY so the
 *    caller can retry or back off. Making the stack properly
 *    reentrant is the real fix and is still deferred - see the note at
 *    the bottom of this file - but the lock is now held for short
 *    slices, not for a whole transfer, which is what makes a server
 *    with several clients workable on top of it.
 *
 * 4. Ownership. Every TCP/listener handle records the process that
 *    created or accepted it. A handle whose owner is gone is closed by
 *    net_sweep_dead_owners(), so a crashed or killed program cannot
 *    leak the table dry - which matters once a server starts one
 *    process per client.
 */

#define NET_MAX_TCP_HANDLES  TCP_MAX_CONNS
#define NET_MAX_LISTENERS    TCP_MAX_LISTENERS
#define NET_MAX_UDP_BINDINGS 4      // udp.c allows 8; 4 trampolines is plenty
#define NET_UDP_QUEUE_DEPTH  8
#define NET_UDP_DGRAM_MAX    1024   // discovery + DNS sized; larger is dropped

// Longest a single SYS_NET_TCP_SEND may hold the stack. tcp_send() blocks
// until the peer ACKs, so an unbounded send from one process would keep
// every other process out for the whole transfer. Userland already
// handles a short count (mos_tcp_send_all / dlr_tcp_write loop), so the
// cap costs nothing but a few extra syscalls and lets other processes
// take a turn between slices.
#define NET_TCP_SEND_SLICE   4096

// How long an op waits for another process to leave the stack before
// giving up with SYS_ERR_BUSY.
#define NET_CLAIM_WAIT_MS    250

// Minimum spacing between dead-owner sweeps (each one walks the process
// list).
#define NET_SWEEP_INTERVAL_MS 250

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

// Waits (yielding, not spinning) for the current holder. tcp_connect()
// and friends already sleep inside the stack, so sleeping here is the
// same kind of blocking, just earlier.
static bool net_claim_wait(void) {
    if (net_try_claim()) return true;
    for (uint32_t waited = 0; waited < NET_CLAIM_WAIT_MS; waited++) {
        sleep(1);
        if (net_try_claim()) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// TCP handle table
// ---------------------------------------------------------------------------

typedef struct {
    bool in_use;
    tcp_conn_t* conn;
    uint64_t owner_pid;         // 0 = unowned (never swept)
} net_tcp_slot_t;

static net_tcp_slot_t g_tcp_slots[NET_MAX_TCP_HANDLES];

// Handles are 1-based so 0 is always invalid.
static uint64_t tcp_slot_alloc(tcp_conn_t* conn) {
    for (int i = 0; i < NET_MAX_TCP_HANDLES; i++) {
        if (!g_tcp_slots[i].in_use) {
            g_tcp_slots[i].in_use = true;
            g_tcp_slots[i].conn = conn;
            g_tcp_slots[i].owner_pid = getCurrentPID();
            return (uint64_t)(i + 1);
        }
    }
    return 0;
}

static int tcp_slot_free_count(void) {
    int n = 0;
    for (int i = 0; i < NET_MAX_TCP_HANDLES; i++) if (!g_tcp_slots[i].in_use) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Listener table
// ---------------------------------------------------------------------------
//
// Separate from the connection table on purpose: a listener has no
// tcp_conn_t (it is just a port in tcp.c) and its handle must never be
// accepted by SEND/RECV/CLOSE.

typedef struct {
    bool in_use;
    uint16_t port;
    uint64_t owner_pid;
} net_listen_slot_t;

static net_listen_slot_t g_listen_slots[NET_MAX_LISTENERS];

static uint64_t listen_slot_alloc(uint16_t port) {
    for (int i = 0; i < NET_MAX_LISTENERS; i++) {
        if (!g_listen_slots[i].in_use) {
            g_listen_slots[i].in_use = true;
            g_listen_slots[i].port = port;
            g_listen_slots[i].owner_pid = getCurrentPID();
            return (uint64_t)(i + 1);
        }
    }
    return 0;
}

static net_listen_slot_t* listen_slot_get(uint64_t handle) {
    if (handle == 0 || handle > NET_MAX_LISTENERS) return NULL;
    net_listen_slot_t* slot = &g_listen_slots[handle - 1];
    return slot->in_use ? slot : NULL;
}

// ---------------------------------------------------------------------------
// Dead-owner sweep
// ---------------------------------------------------------------------------

static uint64_t g_last_sweep_ms = 0;

static bool pid_is_live(uint64_t pid, process_t** procs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (procs[i]->pid != pid) continue;
        return procs[i]->state != PROCESS_ZOMBIE && procs[i]->state != PROCESS_TERMINATED;
    }
    return false;
}

// Caller must hold the claim (it calls into tcp.c). Closes every handle
// whose owning process no longer exists. A failed process-list
// allocation skips the sweep rather than treating "could not look" as
// "everyone is dead".
static void net_sweep_dead_owners(void) {
    uint64_t now = time_get_uptime_ms();
    if (now - g_last_sweep_ms < NET_SWEEP_INTERVAL_MS) return;
    g_last_sweep_ms = now;

    bool any_owned = false;
    for (int i = 0; i < NET_MAX_TCP_HANDLES; i++)
        if (g_tcp_slots[i].in_use && g_tcp_slots[i].owner_pid) any_owned = true;
    for (int i = 0; i < NET_MAX_LISTENERS; i++)
        if (g_listen_slots[i].in_use && g_listen_slots[i].owner_pid) any_owned = true;
    if (!any_owned) return;

    size_t count = 0;
    process_t** procs = get_procs(&count);
    if (!procs || count == 0) return;

    for (int i = 0; i < NET_MAX_TCP_HANDLES; i++) {
        net_tcp_slot_t* slot = &g_tcp_slots[i];
        if (!slot->in_use || !slot->owner_pid) continue;
        if (pid_is_live(slot->owner_pid, procs, count)) continue;

        serial_write_str("net: reclaiming TCP handle of dead process\n");
        tcp_abort(slot->conn);
        slot->in_use = false;
        slot->conn = NULL;
        slot->owner_pid = 0;
    }

    for (int i = 0; i < NET_MAX_LISTENERS; i++) {
        net_listen_slot_t* slot = &g_listen_slots[i];
        if (!slot->in_use || !slot->owner_pid) continue;
        if (pid_is_live(slot->owner_pid, procs, count)) continue;

        serial_write_str("net: reclaiming listener of dead process\n");
        tcp_unlisten(slot->port);
        slot->in_use = false;
        slot->owner_pid = 0;
    }

    free_mem(procs);
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
        case TCP_SYN_SENT:
        case TCP_SYN_RCVD:    return SYSCALL_NET_TCP_CONNECTING;
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
    memset(g_listen_slots, 0, sizeof(g_listen_slots));
    memset(g_udp_bindings, 0, sizeof(g_udp_bindings));
    g_net_busy = 0;
    g_last_sweep_ms = 0;
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

    if (!net_claim_wait()) return SYS_ERR_BUSY;
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
            net_sweep_dead_owners();

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

            // One slice per call: see NET_TCP_SEND_SLICE. The caller
            // sees a short count and comes back for the rest.
            uint32_t slice = request->len;
            if (slice > NET_TCP_SEND_SLICE) slice = NET_TCP_SEND_SLICE;

            int32_t sent = tcp_send(slot->conn, request->buf, slice,
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

        case SYS_NET_TCP_LISTEN: {
            if (!request->out_handle || request->local_port == 0) { result = SYS_ERR_INVAL; break; }
            if (!ip_is_configured()) { result = SYS_ERR_GENERIC; break; }
            net_sweep_dead_owners();

            // Already listening (this process or another): no port
            // sharing, same as UDP bind.
            if (tcp_is_listening(request->local_port)) { result = SYS_ERR_INVAL; break; }

            uint64_t handle = listen_slot_alloc(request->local_port);
            if (handle == 0) { result = SYS_ERR_GENERIC; break; }

            if (!tcp_listen(request->local_port, request->len)) {
                g_listen_slots[handle - 1].in_use = false;
                result = SYS_ERR_GENERIC;
                break;
            }
            *request->out_handle = handle;
            result = SYS_SUCCESS;
            break;
        }

        case SYS_NET_TCP_ACCEPT: {
            net_listen_slot_t* ls = listen_slot_get(request->handle);
            if (!ls) { result = SYS_ERR_BADFD; break; }
            if (!request->out_handle) { result = SYS_ERR_INVAL; break; }
            net_sweep_dead_owners();

            // Check for room BEFORE accepting: tcp_accept() removes the
            // connection from the backlog, and a connection with no
            // handle to carry it would be unreachable and never closed.
            if (tcp_slot_free_count() == 0) { result = SYS_ERR_AGAIN; break; }

            // Non-blocking by design. Holding the stack while waiting
            // for a SYN would lock out the very connections that
            // accept's caller is trying to serve. Pump the NIC once
            // and report whatever is there.
            network_manager_poll();

            uint32_t rip = 0;
            uint16_t rport = 0;
            tcp_conn_t* conn = tcp_accept(ls->port, &rip, &rport);
            if (!conn) { result = SYS_ERR_AGAIN; break; }

            uint64_t handle = tcp_slot_alloc(conn);   // cannot fail: room checked above
            *request->out_handle = handle;
            if (request->out_from_ip) *request->out_from_ip = rip;
            if (request->out_from_port) *request->out_from_port = rport;
            result = SYS_SUCCESS;
            break;
        }

        case SYS_NET_TCP_UNLISTEN: {
            net_listen_slot_t* ls = listen_slot_get(request->handle);
            if (!ls) { result = SYS_ERR_BADFD; break; }
            tcp_unlisten(ls->port);
            ls->in_use = false;
            ls->owner_pid = 0;
            result = SYS_SUCCESS;
            break;
        }

        case SYS_NET_TCP_HANDOFF: {
            net_tcp_slot_t* slot = tcp_slot_get(request->handle);
            if (!slot) { result = SYS_ERR_BADFD; break; }
            if (request->len == 0) { result = SYS_ERR_INVAL; break; }
            // Only the owner may give a connection away. Besides the
            // obvious, this is what makes a stale handle number safe: if
            // the slot was closed and reused by someone else in the
            // meantime, its owner is no longer the caller.
            if (slot->owner_pid != getCurrentPID()) { result = SYS_ERR_PERM; break; }
            slot->owner_pid = (uint64_t)request->len;
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
 * - Reentrancy. The claim is a guard, not a solution. Making
 *   ip_send()/udp_send()/tcp_send_segment() use per-call buffers (or a
 *   small pool) would let two processes use the network at once. What
 *   changed is only the hold time: sends are sliced (NET_TCP_SEND_SLICE)
 *   and accept never blocks, so one process cannot keep the others out
 *   for a whole transfer. It is still true that the terminal's own net
 *   commands (dhcp, wget) call into the stack WITHOUT this claim, so
 *   running them while a server is busy can still corrupt a frame.
 *
 * - Ownership is swept, not hooked. Handles of a dead process are
 *   reclaimed lazily by net_sweep_dead_owners() the next time any
 *   process connects, listens or accepts, rather than from
 *   process_exit(). Until then a dead process's connection is still
 *   ESTABLISHED as far as the peer can tell (it is reset when swept).
 *   Hooking process_exit() would close that gap.
 *
 * - Nested polling. arp_resolve() polls from inside receive handlers.
 *   eth_learn_sender() makes the common case a cache hit; an off-link
 *   peer whose gateway entry has been evicted can still trigger it.
 */
