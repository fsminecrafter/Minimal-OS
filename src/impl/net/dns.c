#include "net/dns.h"
#include "net/udp.h"
#include "net/ip.h"
#include "x86_64/network_manager.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "string.h"
#include "serial.h"

#define DNS_PORT 53
#define DNS_CLIENT_PORT 40053

typedef struct __attribute__((packed)) {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} dns_header_t;

static volatile bool g_dns_got_answer = false;
static uint32_t g_dns_result;
static uint16_t g_dns_id;

static uint8_t* dns_encode_name(uint8_t* p, const char* hostname) {
    const char* label_start = hostname;
    while (true) {
        const char* c = label_start;
        while (*c && *c != '.') c++;
        uint8_t label_len = (uint8_t)(c - label_start);
        *p++ = label_len;
        memcpy(p, label_start, label_len);
        p += label_len;
        if (*c == '\0') break;
        label_start = c + 1;
    }
    *p++ = 0;
    return p;
}

static void dns_handle_reply(uint32_t src_ip, uint16_t src_port,
                             const uint8_t* data, uint16_t len) {
    (void)src_ip; (void)src_port;
    if (len < sizeof(dns_header_t)) return;
    const dns_header_t* hdr = (const dns_header_t*)data;
    if (net_ntohs(hdr->id) != g_dns_id) return;
    uint16_t ancount = net_ntohs(hdr->ancount);
    if (ancount == 0) return;

    const uint8_t* p = data + sizeof(dns_header_t);
    const uint8_t* end = data + len;

    uint16_t qdcount = net_ntohs(hdr->qdcount);
    for (uint16_t i = 0; i < qdcount && p < end; i++) {
        while (p < end && *p) {
            if ((*p & 0xC0) == 0xC0) { p += 2; goto qdone; }
            p += *p + 1;
        }
        p++;
        qdone:;
        p += 4;
    }

    for (uint16_t i = 0; i < ancount && p + 12 <= end; i++) {
        if ((*p & 0xC0) == 0xC0) p += 2;
        else { while (p < end && *p) p += *p + 1; p++; }

        uint16_t rtype = net_ntohs(*(const uint16_t*)p); p += 2;
        p += 2; // class
        p += 4; // ttl
        uint16_t rdlen = net_ntohs(*(const uint16_t*)p); p += 2;

        if (rtype == 1 && rdlen == 4 && p + 4 <= end) {
            g_dns_result = net_ntohl(*(const uint32_t*)p);
            g_dns_got_answer = true;
            return;
        }
        p += rdlen;
    }
}

bool dns_resolve(const char* hostname, uint32_t* out_ip, uint32_t timeout_ms) {
    uint32_t dns_server = ip_get_dns();
    if (dns_server == 0) {
        serial_write_str("DNS: no DNS server configured\n");
        return false;
    }

    static uint8_t query[512];
    dns_header_t* hdr = (dns_header_t*)query;
    g_dns_id = (uint16_t)time_get_uptime_ms();
    hdr->id = net_htons(g_dns_id);
    hdr->flags = net_htons(0x0100);
    hdr->qdcount = net_htons(1);
    hdr->ancount = 0; hdr->nscount = 0; hdr->arcount = 0;

    uint8_t* p = query + sizeof(dns_header_t);
    p = dns_encode_name(p, hostname);
    *p++ = 0x00; *p++ = 0x01; // QTYPE A
    *p++ = 0x00; *p++ = 0x01; // QCLASS IN

    uint16_t query_len = (uint16_t)(p - query);

    g_dns_got_answer = false;
    udp_register_handler(DNS_CLIENT_PORT, dns_handle_reply);
    udp_send(dns_server, DNS_CLIENT_PORT, DNS_PORT, query, query_len);

    uint64_t start = time_get_uptime_ms();
    while (!g_dns_got_answer && time_get_uptime_ms() - start < timeout_ms) {
        network_manager_poll();
        sleep(10);
    }
    udp_unregister_handler(DNS_CLIENT_PORT);

    if (!g_dns_got_answer) {
        serial_write_str("DNS: resolve timed out\n");
        return false;
    }
    if (out_ip) *out_ip = g_dns_result;
    return true;
}
