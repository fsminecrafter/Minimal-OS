#include "net/dhcp.h"
#include "net/udp.h"
#include "net/ip.h"
#include "net/arp.h"
#include "net/ethernet.h"
#include "x86_64/network_manager.h"
#include "x86_64/net_syscall.h"
#include "x86_64/scheduler.h"
#include "x86_64/proc.h"
#include "time.h"
#include "string.h"
#include "serial.h"
#include <stddef.h>

/*
 * DHCP client (RFC 2131) that works both behind a home router and on a
 * line straight to an ISP. ISP servers differ from routers in ways that
 * matter here:
 *  - they answer through relays, often unicast to the offered address
 *    whatever the broadcast flag says (or only when it is clear), so the
 *    flag alternates between attempts;
 *  - some only answer clients that send a client identifier (option 61)
 *    or a host name, and want the max message size;
 *  - the router can be outside the offered subnet (a /32 or /30 with the
 *    gateway elsewhere), given as a classless static route (option 121 or
 *    Microsoft's 249) instead of option 3;
 *  - leases are short and the line stops working when one runs out, so a
 *    background task renews at T1, rebinds at T2 and starts over when the
 *    lease expires;
 *  - they can take several seconds to answer, so attempts back off
 *    instead of giving up after a few tries.
 */

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC_COOKIE 0x63825363u
#define DHCP_OP_REQUEST 1
#define DHCP_HTYPE_ETH  1

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7

#define DHCP_HOSTNAME "minimal-os"

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
    uint8_t  options[312];
} dhcp_packet_t;

typedef struct {
    uint32_t ip, netmask, router, dns[2], server;
    uint32_t lease_s, t1_s, t2_s;   /* 0: not given */
    uint16_t mtu;
} dhcp_lease_t;

/* how a REQUEST is sent */
enum { REQ_SELECTING, REQ_RENEWING, REQ_REBINDING };

static volatile bool g_offer_received;
static volatile bool g_ack_received;
static volatile bool g_nak_received;
static dhcp_lease_t g_offer;      /* filled by the OFFER, then by the ACK */
static uint32_t g_xid;
static uint32_t g_replies_seen, g_replies_other_xid;
static uint32_t g_attempt;        /* DISCOVERs sent in this acquisition */
static uint64_t g_started_ms;     /* for the secs field */

/* the lease in use */
static dhcp_lease_t g_lease;
static uint64_t g_bound_ms;       /* when g_lease was (re)confirmed */
static volatile bool g_bound;

static volatile int g_dhcp_busy;  /* one exchange at a time */

static uint8_t* dhcp_put_option(uint8_t* p, uint8_t code, uint8_t len, const void* data) {
    *p++ = code;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

static void dhcp_send(uint8_t msg_type, int mode, uint32_t requested_ip, uint32_t server_ip) {
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = DHCP_OP_REQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = 6;
    pkt.xid = net_htonl(g_xid);
    uint64_t elapsed = (time_get_uptime_ms() - g_started_ms) / 1000;
    pkt.secs = net_htons((uint16_t)(elapsed > 0xFFFF ? 0xFFFF : elapsed));
    /* With no address yet, ask for broadcast replies on even attempts and
     * unicast ones on odd attempts: some relays only deliver one kind. */
    bool have_address = msg_type == DHCPREQUEST && mode != REQ_SELECTING;
    if (!have_address && msg_type != DHCPRELEASE && (g_attempt & 1) == 0)
        pkt.flags = net_htons(0x8000);
    if (have_address || msg_type == DHCPRELEASE) pkt.ciaddr = net_htonl(g_lease.ip);
    memcpy(pkt.chaddr, g_local_mac, 6);
    pkt.magic_cookie = net_htonl(DHCP_MAGIC_COOKIE);

    uint8_t* p = pkt.options;
    p = dhcp_put_option(p, 53, 1, &msg_type);
    uint8_t client_id[7] = {DHCP_HTYPE_ETH};
    memcpy(client_id + 1, g_local_mac, 6);
    p = dhcp_put_option(p, 61, sizeof(client_id), client_id);
    if (msg_type == DHCPREQUEST && mode == REQ_SELECTING) {
        uint32_t req_ip_be = net_htonl(requested_ip);
        p = dhcp_put_option(p, 50, 4, &req_ip_be);
        uint32_t server_be = net_htonl(server_ip);
        p = dhcp_put_option(p, 54, 4, &server_be);
    }
    if (msg_type == DHCPRELEASE) {
        uint32_t server_be = net_htonl(server_ip);
        p = dhcp_put_option(p, 54, 4, &server_be);
    } else {
        uint16_t max_size = net_htons(1500);
        p = dhcp_put_option(p, 57, 2, &max_size);
        p = dhcp_put_option(p, 12, (uint8_t)(sizeof(DHCP_HOSTNAME) - 1), DHCP_HOSTNAME);
        /* subnet, router, DNS, domain, MTU, broadcast, lease, server id,
         * T1, T2, classless routes (RFC 3442 and Microsoft's) */
        static const uint8_t param_list[] = {1, 3, 6, 15, 26, 28, 51, 54, 58, 59, 121, 249};
        p = dhcp_put_option(p, 55, sizeof(param_list), param_list);
    }
    *p++ = 0xFF;

    uint16_t opt_len = (uint16_t)(p - pkt.options);
    uint16_t total_len = (uint16_t)(offsetof(dhcp_packet_t, options) + opt_len);
    // Pad to the 300-byte BOOTP minimum (RFC 1542); some routers drop
    // shorter requests. The options area is already zeroed (= pad bytes).
    if (total_len < 300) total_len = 300;

    /* renewals go straight to the server that gave the lease; everything
     * else is broadcast (a REQUEST when selecting tells the other servers
     * their offers were declined) */
    uint32_t dst = (msg_type == DHCPREQUEST && mode == REQ_RENEWING) || msg_type == DHCPRELEASE
                       ? server_ip : 0xFFFFFFFFu;
    udp_send(dst, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &pkt, total_len);
}

/* Options can continue in the file and sname fields (option 52). */
typedef struct {
    const uint8_t* start[3];
    const uint8_t* end[3];
    int count;
} dhcp_areas_t;

static const uint8_t* find_in_area(const uint8_t* p, const uint8_t* end, uint8_t code,
                                   uint8_t* out_len) {
    while (p < end && *p != 0xFF) {
        if (*p == 0) { p++; continue; }
        uint8_t c = *p++;
        if (p >= end) break;
        uint8_t len = *p++;
        if (p + len > end) break;
        if (c == code) { if (out_len) *out_len = len; return p; }
        p += len;
    }
    return NULL;
}

static void dhcp_areas(const dhcp_packet_t* pkt, uint16_t opt_len, dhcp_areas_t* a) {
    a->count = 0;
    a->start[a->count] = pkt->options;
    a->end[a->count++] = pkt->options + opt_len;
    uint8_t len = 0;
    const uint8_t* overload = find_in_area(pkt->options, pkt->options + opt_len, 52, &len);
    if (overload && len == 1) {
        if (*overload & 1) { a->start[a->count] = pkt->file; a->end[a->count++] = pkt->file + sizeof(pkt->file); }
        if (*overload & 2) { a->start[a->count] = pkt->sname; a->end[a->count++] = pkt->sname + sizeof(pkt->sname); }
    }
}

static const uint8_t* dhcp_find_option(const dhcp_areas_t* a, uint8_t code, uint8_t* out_len) {
    for (int i = 0; i < a->count; i++) {
        const uint8_t* found = find_in_area(a->start[i], a->end[i], code, out_len);
        if (found) return found;
    }
    return NULL;
}

static uint32_t read_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The default route from a classless static route option, or 0. */
static uint32_t classless_default_route(const uint8_t* p, uint8_t len) {
    const uint8_t* end = p + len;
    while (p < end) {
        uint8_t width = *p++;
        if (width > 32) return 0;
        uint8_t octets = (uint8_t)((width + 7) / 8);
        if (p + octets + 4 > end) return 0;
        p += octets;
        uint32_t router = read_be32(p);
        p += 4;
        if (width == 0) return router;
    }
    return 0;
}

static void dhcp_read_options(const dhcp_areas_t* a, dhcp_lease_t* l) {
    uint8_t len;
    const uint8_t* o;
    if ((o = dhcp_find_option(a, 1, &len)) && len == 4) l->netmask = read_be32(o);
    if ((o = dhcp_find_option(a, 3, &len)) && len >= 4) l->router = read_be32(o);
    /* RFC 3442: when classless routes are given, option 3 is ignored */
    uint32_t classless = 0;
    if ((o = dhcp_find_option(a, 121, &len))) classless = classless_default_route(o, len);
    if (!classless && (o = dhcp_find_option(a, 249, &len))) classless = classless_default_route(o, len);
    if (classless) l->router = classless;
    if ((o = dhcp_find_option(a, 6, &len)) && len >= 4) {
        l->dns[0] = read_be32(o);
        l->dns[1] = len >= 8 ? read_be32(o + 4) : 0;
    }
    if ((o = dhcp_find_option(a, 51, &len)) && len == 4) l->lease_s = read_be32(o);
    if ((o = dhcp_find_option(a, 58, &len)) && len == 4) l->t1_s = read_be32(o);
    if ((o = dhcp_find_option(a, 59, &len)) && len == 4) l->t2_s = read_be32(o);
    if ((o = dhcp_find_option(a, 26, &len)) && len == 2) l->mtu = (uint16_t)(o[0] << 8 | o[1]);
    if ((o = dhcp_find_option(a, 54, &len)) && len == 4) l->server = read_be32(o);
}

static void dhcp_handle_reply(uint32_t src_ip, uint16_t src_port,
                              const uint8_t* data, uint16_t len) {
    (void)src_port;
    g_replies_seen++;
    if (len < offsetof(dhcp_packet_t, options)) return;
    const dhcp_packet_t* pkt = (const dhcp_packet_t*)data;
    if (pkt->op != 2) return;   /* BOOTREPLY */
    if (net_ntohl(pkt->xid) != g_xid) { g_replies_other_xid++; return; }
    if (memcmp(pkt->chaddr, g_local_mac, 6) != 0) return;
    if (net_ntohl(pkt->magic_cookie) != DHCP_MAGIC_COOKIE) return;

    uint16_t opt_len = (uint16_t)(len - offsetof(dhcp_packet_t, options));
    dhcp_areas_t areas;
    dhcp_areas(pkt, opt_len, &areas);
    uint8_t type_len = 0;
    const uint8_t* type = dhcp_find_option(&areas, 53, &type_len);
    if (!type || type_len < 1) return;

    if (*type == DHCPOFFER) {
        if (g_offer_received || pkt->yiaddr == 0) return;   /* take the first offer */
        memset(&g_offer, 0, sizeof(g_offer));
        g_offer.netmask = 0;
        g_offer.ip = net_ntohl(pkt->yiaddr);
        dhcp_read_options(&areas, &g_offer);
        if (!g_offer.server) g_offer.server = pkt->siaddr ? net_ntohl(pkt->siaddr) : src_ip;
        g_offer_received = true;
    } else if (*type == DHCPACK) {
        if (pkt->yiaddr) g_offer.ip = net_ntohl(pkt->yiaddr);
        dhcp_read_options(&areas, &g_offer);
        g_ack_received = true;
    } else if (*type == DHCPNAK) {
        g_nak_received = true;
    }
}

/* Waits up to ms for one of the flags, polling the card. */
static void dhcp_wait(uint32_t ms, volatile bool* a, volatile bool* b) {
    uint64_t start = time_get_uptime_ms();
    while (!*a && !(b && *b) && time_get_uptime_ms() - start < ms) {
        network_manager_poll();
        sleep(10);
    }
}

static uint32_t default_netmask(uint32_t ip) {
    uint32_t first = ip >> 24;
    if (first < 128) return 0xFF000000u;
    if (first < 192) return 0xFFFF0000u;
    return 0xFFFFFF00u;
}

static void dhcp_apply(const dhcp_lease_t* l) {
    uint32_t mask = l->netmask ? l->netmask : default_netmask(l->ip);
    ip_configure(l->ip, mask, l->router);
    ip_set_dns(l->dns[0] ? l->dns[0] : l->dns[1]);
    /* tell the upstream router/switch where this address lives now */
    arp_send_request(l->ip);
}

static void dhcp_log_lease(const dhcp_lease_t* l, bool quiet) {
    if (quiet) return;
    char ip[16], mask[16], gw[16], dns[16], line[160];
    ip_to_string(l->ip, ip);
    ip_to_string(ip_get_netmask(), mask);
    ip_to_string(l->router, gw);
    ip_to_string(ip_get_dns(), dns);
    snprintf(line, sizeof(line), "DHCP: bound %s mask %s gateway %s dns %s lease %us\n",
             ip, mask, gw, dns, (unsigned)l->lease_s);
    serial_write_str(line);
}

static void dhcp_keeper_ensure(void);

/* DISCOVER -> OFFER -> REQUEST -> ACK within timeout_ms. */
static bool dhcp_acquire_internal(uint32_t timeout_ms, bool quiet) {
    if (__atomic_exchange_n(&g_dhcp_busy, 1, __ATOMIC_ACQUIRE)) {
        /* another acquisition or renewal is running: wait for its result */
        uint64_t start = time_get_uptime_ms();
        while (__atomic_load_n(&g_dhcp_busy, __ATOMIC_ACQUIRE) &&
               time_get_uptime_ms() - start < timeout_ms)
            sleep(20);
        return g_bound && ip_get_local() == g_lease.ip;
    }
    g_replies_seen = g_replies_other_xid = 0;
    g_started_ms = time_get_uptime_ms();
    udp_register_handler(DHCP_CLIENT_PORT, dhcp_handle_reply);

    bool bound = false;
    uint64_t deadline = g_started_ms + timeout_ms;
    g_attempt = 0;
    while (!bound && time_get_uptime_ms() < deadline) {
        g_offer_received = g_ack_received = g_nak_received = false;
        g_xid = (uint32_t)time_get_uptime_ms() * 2654435761u ^ 0xA5A5A5A5u ^
                ((uint32_t)g_local_mac[4] << 8 | g_local_mac[5]);

        /* DISCOVER with backoff: 2 s, 4 s, then every 6 s. The first
         * frames are often lost while the link (or the ISP's modem)
         * is still coming up. */
        while (!g_offer_received && time_get_uptime_ms() < deadline) {
            if (!quiet) serial_write_str("DHCP: sending DISCOVER\n");
            dhcp_send(DHCPDISCOVER, REQ_SELECTING, 0, 0);
            uint32_t wait = g_attempt == 0 ? 2000 : g_attempt == 1 ? 4000 : 6000;
            g_attempt++;
            uint64_t left = deadline - time_get_uptime_ms();
            dhcp_wait(wait < left ? wait : (uint32_t)left, &g_offer_received, NULL);
        }
        if (!g_offer_received) break;

        if (!quiet) serial_write_str("DHCP: got OFFER, sending REQUEST\n");
        for (int tries = 0; tries < 4 && !g_ack_received && !g_nak_received &&
                            time_get_uptime_ms() < deadline; tries++) {
            dhcp_send(DHCPREQUEST, REQ_SELECTING, g_offer.ip, g_offer.server);
            dhcp_wait(tries < 2 ? 2000 : 4000, &g_ack_received, &g_nak_received);
        }
        if (g_ack_received) {
            bound = true;
        } else if (!quiet) {
            serial_write_str(g_nak_received ? "DHCP: request refused (NAK), starting over\n"
                                            : "DHCP: no ACK, starting over\n");
        }
    }
    udp_unregister_handler(DHCP_CLIENT_PORT);

    if (bound) {
        g_lease = g_offer;
        g_bound_ms = time_get_uptime_ms();
        g_bound = true;
        dhcp_apply(&g_lease);
        dhcp_log_lease(&g_lease, quiet);
    } else if (!quiet) {
        serial_write_str("DHCP: no lease\n");
    }
    __atomic_store_n(&g_dhcp_busy, 0, __ATOMIC_RELEASE);
    if (bound) dhcp_keeper_ensure();
    return bound;
}

bool dhcp_acquire(uint32_t timeout_ms) {
    return dhcp_acquire_internal(timeout_ms, false);
}

bool dhcp_acquire_quiet(uint32_t timeout_ms) {
    return dhcp_acquire_internal(timeout_ms, true);
}

void dhcp_last_stats(uint32_t* replies, uint32_t* other_xid) {
    *replies = g_replies_seen;
    *other_xid = g_replies_other_xid;
}

/* ---------------- lease keeper ----------------
 * One kernel process per boot. It keeps retrying while there is no
 * address (when started by `dhcp --background`) and renews the lease. */

static volatile bool g_keeper_running;
static volatile bool g_keeper_wanted;   /* stays on until dhcp_stop() */
static volatile bool g_keeper_retry;    /* keep trying while unbound */

/* One renewal/rebind exchange; returns 1 on ACK, -1 on NAK, 0 otherwise. */
static int dhcp_renew_once(int mode) {
    if (__atomic_exchange_n(&g_dhcp_busy, 1, __ATOMIC_ACQUIRE)) return 0;
    if (!net_stack_claim()) {
        __atomic_store_n(&g_dhcp_busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    g_ack_received = g_nak_received = false;
    g_offer = g_lease;
    g_xid = (uint32_t)time_get_uptime_ms() * 2654435761u ^ 0x5A5A5A5Au;
    g_started_ms = time_get_uptime_ms();
    udp_register_handler(DHCP_CLIENT_PORT, dhcp_handle_reply);
    dhcp_send(DHCPREQUEST, mode, 0, g_lease.server);
    net_stack_release();
    /* the timer interrupt keeps polling the card: wait without the claim */
    uint64_t start = time_get_uptime_ms();
    while (!g_ack_received && !g_nak_received && time_get_uptime_ms() - start < 5000) sleep(20);
    int result = g_ack_received ? 1 : g_nak_received ? -1 : 0;
    if (net_stack_claim()) {
        udp_unregister_handler(DHCP_CLIENT_PORT);
        net_stack_release();
    }
    if (result == 1) {
        g_lease = g_offer;
        g_bound_ms = time_get_uptime_ms();
        if (ip_get_local() != g_lease.ip || ip_get_gateway() != g_lease.router) dhcp_apply(&g_lease);
        else ip_set_dns(g_lease.dns[0] ? g_lease.dns[0] : g_lease.dns[1]);
    }
    __atomic_store_n(&g_dhcp_busy, 0, __ATOMIC_RELEASE);
    return result;
}

static void dhcp_drop_lease(const char* why) {
    serial_write_str("DHCP: ");
    serial_write_str(why);
    serial_write_str(", requesting a new address\n");
    g_bound = false;
    if (ip_get_local() == g_lease.ip) {
        ip_configure(0, 0, 0);
        ip_set_dns(0);
    }
}

static void dhcp_keeper(void) {
    uint32_t backoff_s = 5;
    for (;;) {
        if (!g_keeper_wanted) {
            sleep(1000);
            continue;
        }
        if (!g_bound) {
            if (!g_keeper_retry) {
                sleep(1000);
                continue;
            }
            bool ok = false;
            if (net_stack_claim()) {
                ok = dhcp_acquire_internal(30000, true);
                net_stack_release();
            }
            if (ok) {
                backoff_s = 5;
            } else {
                sleep((uint64_t)backoff_s * 1000);
                if (backoff_s < 60) backoff_s *= 2;
            }
            continue;
        }
        /* bound: someone set a static address (or another lease)? */
        if (ip_get_local() != g_lease.ip) {
            g_bound = false;
            continue;
        }
        uint32_t lease = g_lease.lease_s;
        if (lease == 0 || lease == 0xFFFFFFFFu) {   /* infinite */
            sleep(5000);
            continue;
        }
        uint64_t t1 = (uint64_t)(g_lease.t1_s ? g_lease.t1_s : lease / 2) * 1000;
        uint64_t t2 = (uint64_t)(g_lease.t2_s ? g_lease.t2_s : lease / 8 * 7) * 1000;
        uint64_t end = (uint64_t)lease * 1000;
        uint64_t age = time_get_uptime_ms() - g_bound_ms;
        if (age < t1) {
            uint64_t wait = t1 - age;
            sleep(wait > 5000 ? 5000 : wait);
            continue;
        }
        if (age >= end) {
            dhcp_drop_lease("lease expired");
            g_keeper_retry = true;
            continue;
        }
        int r = dhcp_renew_once(age < t2 ? REQ_RENEWING : REQ_REBINDING);
        if (r == 1) continue;
        if (r == -1) {
            dhcp_drop_lease("lease refused");
            g_keeper_retry = true;
            continue;
        }
        /* RFC 2131: retry at half the time left until T2 (or the end),
         * but not more often than every 60 s */
        uint64_t limit = age < t2 ? t2 : end;
        uint64_t wait = (limit - age) / 2;
        if (wait < 60000) wait = limit - age < 60000 ? limit - age : 60000;
        if (wait < 1000) wait = 1000;
        sleep(wait);
    }
}

static void dhcp_keeper_start(void) {
    g_keeper_wanted = true;
    if (__atomic_exchange_n(&g_keeper_running, true, __ATOMIC_ACQ_REL)) return;
    if (!proc_create("dhcp", dhcp_keeper)) g_keeper_running = false;
}

static void dhcp_keeper_ensure(void) {
    dhcp_keeper_start();
}

void dhcp_start_background(void) {
    g_keeper_retry = true;
    dhcp_keeper_start();
}

void dhcp_stop(void) {
    g_keeper_wanted = false;
    g_keeper_retry = false;
    g_bound = false;
}

bool dhcp_lease_info(uint32_t* lease_s, uint32_t* remaining_s, uint32_t* server) {
    if (!g_bound || ip_get_local() != g_lease.ip) return false;
    uint64_t age = (time_get_uptime_ms() - g_bound_ms) / 1000;
    if (lease_s) *lease_s = g_lease.lease_s;
    if (remaining_s)
        *remaining_s = g_lease.lease_s == 0xFFFFFFFFu ? 0xFFFFFFFFu
                     : age >= g_lease.lease_s ? 0 : g_lease.lease_s - (uint32_t)age;
    if (server) *server = g_lease.server;
    return true;
}
