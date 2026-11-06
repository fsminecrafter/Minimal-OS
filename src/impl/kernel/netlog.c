#include "x86_64/netlog.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/smp.h"
#include "x86_64/net_syscall.h"
#include "net/ip.h"
#include "net/udp.h"
#include "net/dhcp.h"
#include "string.h"
#include "time.h"

/* See netlog.h. One ring buffer filled by netlog_putc() from anywhere,
 * drained by the "netlog" process, which sends it in UDP datagrams. */

#define NETLOG_RING   (512u * 1024u)        /* power of two */
#define NETLOG_CHUNK  1200u                 /* bytes per datagram, under one MTU */
#define NETLOG_DHCP_AFTER_MS 20000u         /* nobody else brought the network up */

static char g_ring[NETLOG_RING];
static volatile uint64_t g_write;           /* total bytes ever written */
static uint64_t g_read;                     /* sender only */
static bool g_enabled;
static bool g_started;
static uint32_t g_server_ip;
static uint16_t g_server_port = NETLOG_DEFAULT_PORT;
/* The sender's own core while it is inside the network stack: what the
 * stack logs about our packets is not captured, or every datagram would
 * produce the next one. */
static volatile uint32_t g_sending_cpu = 0xFFFFFFFFu;

bool netlog_enabled(void) { return g_enabled; }

void netlog_putc(char c) {
    if (!g_enabled || c == '\r') return;
    if (g_sending_cpu == smp_current_cpu_id()) return;
    uint64_t at = __atomic_fetch_add(&g_write, 1, __ATOMIC_RELAXED);
    g_ring[at & (NETLOG_RING - 1)] = c;
}

void netlog_configure(const char* arg) {
    if (!arg || !*arg) return;
    char ip[20];
    uint32_t i = 0;
    while (arg[i] && arg[i] != ':' && i + 1 < sizeof(ip)) { ip[i] = arg[i]; i++; }
    ip[i] = 0;
    uint32_t addr = ip_parse(ip);
    if (!addr) return;
    if (arg[i] == ':') {
        uint32_t port = 0;
        for (const char* p = arg + i + 1; *p >= '0' && *p <= '9'; p++) port = port * 10 + (uint32_t)(*p - '0');
        if (port > 0 && port < 65536) g_server_port = (uint16_t)port;
    }
    g_server_ip = addr;
    g_enabled = true;
}

static bool send_chunk(const char* data, uint16_t len) {
    if (!net_stack_claim()) return false;
    g_sending_cpu = smp_current_cpu_id();
    bool ok = udp_send(g_server_ip, g_server_port, g_server_port, data, len);
    g_sending_cpu = 0xFFFFFFFFu;
    net_stack_release();
    return ok;
}

static void netlog_worker(void) {
    static char chunk[NETLOG_CHUNK + 64];
    uint64_t started = time_get_uptime_ms();
    bool tried_dhcp = false;
    bool said_hello = false;
    for (;;) {
        sleep(50);
        if (!ip_is_configured()) {
            if (!tried_dhcp && time_get_uptime_ms() - started > NETLOG_DHCP_AFTER_MS) {
                tried_dhcp = true;
                if (net_stack_claim()) {
                    g_sending_cpu = smp_current_cpu_id();
                    dhcp_acquire_quiet(10000);
                    g_sending_cpu = 0xFFFFFFFFu;
                    net_stack_release();
                }
            }
            continue;
        }
        if (!said_hello) {
            static const char hello[] = "\n===== Minimal-OS log (build " BUILD_DATE ") =====\n";
            if (!send_chunk(hello, sizeof(hello) - 1)) continue;
            said_hello = true;
        }
        uint64_t end = __atomic_load_n(&g_write, __ATOMIC_ACQUIRE);
        if (end - g_read > NETLOG_RING) {
            /* lost what was overwritten before the network came up */
            g_read = end - NETLOG_RING + NETLOG_CHUNK;
            static const char lost[] = "\n[netlog: older lines were dropped]\n";
            send_chunk(lost, sizeof(lost) - 1);
        }
        while (g_read < end) {
            uint32_t n = (uint32_t)(end - g_read);
            if (n > NETLOG_CHUNK) {
                n = NETLOG_CHUNK;
                /* end on a line break when one is near, so lines stay whole */
                for (uint32_t k = n; k > NETLOG_CHUNK / 2; k--)
                    if (g_ring[(g_read + k - 1) & (NETLOG_RING - 1)] == '\n') { n = k; break; }
            }
            for (uint32_t k = 0; k < n; k++) chunk[k] = g_ring[(g_read + k) & (NETLOG_RING - 1)];
            if (!send_chunk(chunk, (uint16_t)n)) break;     /* stack busy: retry later */
            g_read += n;
            sleep(1);                                       /* do not flood the NIC */
        }
    }
}

void netlog_start(void) {
    if (!g_enabled || g_started) return;
    g_started = true;
    if (!proc_create("netlog", netlog_worker)) g_started = false;
}
