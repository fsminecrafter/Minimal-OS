#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/scheduler.h"
#include "x86_64/network_manager.h"
#include "net/ethernet.h"
#include "net/ip.h"
#include "net/tcp.h"
#include "net/tls13.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "x86_64/minimafs.h"
#include "fspaths.h"
#include "time.h"

void cmd_ifconfig(int argc, const char** argv) {
    if (!network_manager_has_driver()) {
        graphics_write_textr("No network interface present\n");
        return;
    }

    uint8_t mac[6];
    network_manager_get_mac(mac);
    char buf[64];

    graphics_write_textr("eth0  HWaddr ");
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    graphics_write_textr(buf);

    if (!ip_is_configured()) {
        graphics_write_textr("      (no IP configured - use 'dhcp')\n");
        return;
    }

    char ip_str[16], mask_str[16], gw_str[16], dns_str[16];
    ip_to_string(ip_get_local(), ip_str);
    ip_to_string(ip_get_netmask(), mask_str);
    ip_to_string(ip_get_gateway(), gw_str);
    ip_to_string(ip_get_dns(), dns_str);

    snprintf(buf, sizeof(buf), "      inet addr:%s  Mask:%s\n", ip_str, mask_str);
    graphics_write_textr(buf);
    snprintf(buf, sizeof(buf), "      Gateway:%s  DNS:%s\n", gw_str, dns_str);
    graphics_write_textr(buf);
}

void cmd_dhcp(int argc, const char** argv) {
    bool background = argc > 1 && strcmp(argv[1], "--background") == 0;
    if (!network_manager_has_driver()) {
        if (!background) graphics_write_textr("No network interface present\n");
        return;
    }
    if (!background) graphics_write_textr("Requesting IP via DHCP...\n");
    if ((background ? dhcp_acquire_quiet : dhcp_acquire)(10000)) {
        if (background) return;
        char ip_str[16];
        ip_to_string(ip_get_local(), ip_str);
        graphics_write_textr("Bound address: ");
        graphics_write_textr(ip_str);
        graphics_write_textr("\n");
    } else {
        if (!background) graphics_write_textr("DHCP failed (no router response?)\n");
    }
}

static void cmd_ifconfig_static(int argc, const char** argv) {
    if (argc != 4) {
        graphics_write_textr("Usage: ifconfig static <ip> <netmask> <gateway>\n");
        return;
    }

    uint32_t ip = ip_parse(argv[1]);
    uint32_t netmask = ip_parse(argv[2]);
    uint32_t gateway = ip_parse(argv[3]);
    if (!ip || !netmask) {
        graphics_write_textr("ifconfig: invalid static network configuration\n");
        return;
    }

    ip_configure(ip, netmask, gateway);
    char ip_text[16];
    ip_to_string(ip, ip_text);
    graphics_write_textr("Bound static address: ");
    graphics_write_textr(ip_text);
    graphics_write_textr("\n");
}

void cmd_ifconfig_dispatch(int argc, const char** argv) {
    if (argc > 1 && strcmp(argv[1], "static") == 0) {
        cmd_ifconfig_static(argc - 1, argv + 1);
        return;
    }
    cmd_ifconfig(argc, argv);
}

static bool parse_url(const char* url, char* host, size_t host_size,
                      uint16_t* port, char* path, size_t path_size, bool* https) {
    const char* p = url;
    *https = false;
    if (strncmp(p, "https://", 8) == 0) { *https = true; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return false;

    size_t hi = 0;
    *port = *https ? 443 : 80;
    while (*p && *p != '/' && *p != ':' && hi + 1 < host_size) host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0) return false;

    if (*p == ':') {
        p++;
        uint16_t val = 0;
        while (*p >= '0' && *p <= '9') { val = (uint16_t)(val * 10 + (*p - '0')); p++; }
        *port = val ? val : (*https ? 443 : 80);
    }

    if (*p == '/') strncpy(path, p, path_size - 1);
    else strncpy(path, "/", path_size - 1);
    path[path_size - 1] = '\0';
    return true;
}

static bool parse_unix_timestamp(const uint8_t* body, uint32_t body_len,
                                 uint64_t* timestamp) {
    const char* key = "\"unix_timestamp\"";
    size_t key_len = strlen(key);
    for (uint32_t i = 0; i + key_len < body_len; i++) {
        if (memcmp(body + i, key, key_len) != 0) continue;

        uint32_t cursor = i + (uint32_t)key_len;
        while (cursor < body_len &&
               (body[cursor] == ' ' || body[cursor] == '\t' || body[cursor] == ':')) {
            cursor++;
        }
        if (cursor == body_len || body[cursor] < '0' || body[cursor] > '9') return false;

        uint64_t value = 0;
        while (cursor < body_len && body[cursor] >= '0' && body[cursor] <= '9') {
            value = value * 10 + (uint64_t)(body[cursor] - '0');
            cursor++;
        }
        *timestamp = value;
        return true;
    }
    return false;
}

void cmd_wget(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: wget <url> [-d|--download <path>]\n");
        return;
    }
    if (!ip_is_configured()) {
        graphics_write_textr("wget: no IP configured (run 'dhcp' first)\n");
        return;
    }

    const char* url = argv[1];
    const char* save_path = NULL;
    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--download") == 0) && i + 1 < argc) {
            save_path = argv[i + 1];
            i++;
        }
    }

    char host[128];
    char path[256];
    uint16_t port;
    bool https;
    if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path), &https)) {
        graphics_write_textr("wget: could not parse URL\n");
        return;
    }
    bool unix_time_request = !save_path && strcmp(host, "timeapi.io") == 0 &&
                             strcmp(path, "/api/v1/time/current/unix") == 0;

    uint32_t target_ip = ip_parse(host);
    if (target_ip == 0) {
        graphics_write_textr("Resolving ");
        graphics_write_textr(host);
        graphics_write_textr("...\n");
        if (!dns_resolve(host, &target_ip, 5000)) {
            graphics_write_textr("wget: DNS resolution failed\n");
            return;
        }
    }

    char ip_str[16];
    ip_to_string(target_ip, ip_str);
    graphics_write_textr("Connecting to ");
    graphics_write_textr(ip_str);
    graphics_write_textr("...\n");

    tcp_conn_t* conn = NULL;
    tls13_client_t* tls = NULL;
    if (https) tls = tls13_connect(target_ip, port, host, 15000);
    else conn = tcp_connect(target_ip, port, 5000);
    if ((!https && !conn) || (https && !tls)) {
        if (https) {
            graphics_write_textr("wget: TLS connection failed (stage: ");
            graphics_write_textr(tls13_last_error());
            graphics_write_textr(")\n");
            serial_write_str("wget: TLS connection failed (stage: ");
            serial_write_str(tls13_last_error());
            serial_write_str(")\n");
        } else graphics_write_textr("wget: connection failed\n");
        return;
    }

    if (https)
        serial_write_str("wget: TLS handshake completed\n");

    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: MinimalOS-wget\r\n\r\n",
        path, host);

    int32_t sent = https ? tls13_send(tls, request, (uint32_t)req_len)
                         : tcp_send(conn, request, (uint32_t)req_len, 5000);
    if (sent != req_len) {
        graphics_write_textr("wget: failed to send request\n");
        if (https) tls13_close(tls); else tcp_close(conn);
        return;
    }

    graphics_write_textr("Downloading...\n");

    minimafs_file_handle_t* out_file = NULL;
    char resolved_path[MINIMAFS_MAX_PATH];
    if (save_path) {
        if (!fs_resolve_path(save_path, resolved_path)) {
            graphics_write_textr("wget: invalid output path\n");
            if (https) tls13_close(tls); else tcp_close(conn);
            return;
        }
        minimafs_file_handle_t* existing = minimafs_open(resolved_path, true);
        if (existing) { minimafs_close(existing); minimafs_delete_file(resolved_path); }
        if (!minimafs_create_file(resolved_path, "binary", "bin")) {
            graphics_write_textr("wget: could not create output file\n");
            if (https) tls13_close(tls); else tcp_close(conn);
            return;
        }
        out_file = minimafs_open(resolved_path, false);
        if (!out_file) {
            graphics_write_textr("wget: could not open output file\n");
            if (https) tls13_close(tls); else tcp_close(conn);
            return;
        }
    }

    bool headers_done = false;
    static uint8_t chunk[1024];
    static uint8_t header_buf[4096];
    uint32_t header_len = 0;
    uint64_t total_bytes = 0;
    bool receive_failed = false;
    static uint8_t unix_time_body[4096];
    uint32_t unix_time_body_len = 0;
    bool unix_time_body_overflow = false;
    const uint64_t IDLE_TIMEOUT_MS = 8000;
    uint64_t last_data_ms = time_get_uptime_ms();

    while (true) {
        int32_t got = https ? tls13_recv(tls, chunk, sizeof(chunk), 8000)
                    : tcp_recv(conn, chunk, sizeof(chunk));
        if (got < 0) {
            if (https && (strcmp(tls13_last_error(), "handshake record authentication") == 0 ||
                          strcmp(tls13_last_error(), "application record authentication") == 0))
                receive_failed = true;
            break;
        }
        if (got == 0) {
            if (time_get_uptime_ms() - last_data_ms > IDLE_TIMEOUT_MS) break;
            tcp_poll();
            sleep(5);
            continue;
        }
        last_data_ms = time_get_uptime_ms();

        const uint8_t* body = chunk;
        uint32_t body_len = (uint32_t)got;
        if (!headers_done) {
            if (header_len + (uint32_t)got > sizeof(header_buf)) {
                graphics_write_textr("wget: HTTP headers too large\n");
                break;
            }
            memcpy(header_buf + header_len, chunk, (uint32_t)got);
            header_len += (uint32_t)got;
            uint32_t header_end = 0;
            for (uint32_t i = 3; i < header_len; i++) {
                if (header_buf[i - 3] == '\r' && header_buf[i - 2] == '\n' &&
                    header_buf[i - 1] == '\r' && header_buf[i] == '\n') {
                    header_end = i + 1;
                    break;
                }
            }
            if (header_end == 0) continue;
            headers_done = true;
            body = header_buf + header_end;
            body_len = header_len - header_end;
        }

        if (body_len > 0) {
            if (unix_time_request) {
                if (unix_time_body_len + body_len > sizeof(unix_time_body)) {
                    unix_time_body_overflow = true;
                } else {
                    memcpy(unix_time_body + unix_time_body_len, body, body_len);
                    unix_time_body_len += body_len;
                }
            } else if (out_file) {
                minimafs_write(out_file, body, body_len);
            } else {
                for (uint32_t i = 0; i < body_len; i++) {
                    graphics_write_textr_char((char)body[i]);
                }
            }
            total_bytes += body_len;
        }
    }

    if (out_file) minimafs_close(out_file);
    if (https) tls13_close(tls); else tcp_close(conn);

    if (receive_failed) {
        graphics_write_textr("wget: TLS receive failed (stage: ");
        graphics_write_textr(tls13_last_error());
        graphics_write_textr(")\n");
        return;
    }
    if (!headers_done) {
        graphics_write_textr("wget: incomplete HTTP response\n");
        return;
    }
    if (unix_time_request) {
        uint64_t timestamp;
        if (unix_time_body_overflow ||
            !parse_unix_timestamp(unix_time_body, unix_time_body_len, &timestamp)) {
            graphics_write_textr("wget: invalid Unix timestamp response\n");
            return;
        }
        graphics_write_textr_udec(timestamp);
        graphics_write_textr("\n");
        return;
    }
    serial_write_str("wget: download completed\n");

    if (save_path) {
        graphics_write_textr("\nSaved ");
        graphics_write_textr_udec(total_bytes);
        graphics_write_textr(" bytes to ");
        graphics_write_textr(save_path);
        graphics_write_textr("\n");
    } else {
        graphics_write_textr("\n");
    }
}

void register_net_commands(void) {
    command_register("ifconfig", cmd_ifconfig_dispatch);
    command_register("dhcp", cmd_dhcp);
    command_register("wget", cmd_wget);
}

REGISTER_COMMAND(register_net_commands);
