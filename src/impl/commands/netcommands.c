#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/network_manager.h"
#include "net/ethernet.h"
#include "net/ip.h"
#include "net/tcp.h"
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
    if (!network_manager_has_driver()) {
        graphics_write_textr("No network interface present\n");
        return;
    }
    graphics_write_textr("Requesting IP via DHCP...\n");
    if (dhcp_acquire(10000)) {
        char ip_str[16];
        ip_to_string(ip_get_local(), ip_str);
        graphics_write_textr("Bound address: ");
        graphics_write_textr(ip_str);
        graphics_write_textr("\n");
    } else {
        graphics_write_textr("DHCP failed (no router response?)\n");
    }
}

static bool parse_url(const char* url, char* host, size_t host_size,
                      uint16_t* port, char* path, size_t path_size) {
    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;

    size_t hi = 0;
    *port = 80;
    while (*p && *p != '/' && *p != ':' && hi + 1 < host_size) host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0) return false;

    if (*p == ':') {
        p++;
        uint16_t val = 0;
        while (*p >= '0' && *p <= '9') { val = (uint16_t)(val * 10 + (*p - '0')); p++; }
        *port = val ? val : 80;
    }

    if (*p == '/') strncpy(path, p, path_size - 1);
    else strncpy(path, "/", path_size - 1);
    path[path_size - 1] = '\0';
    return true;
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
    if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        graphics_write_textr("wget: could not parse URL\n");
        return;
    }

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

    tcp_conn_t* conn = tcp_connect(target_ip, port, 5000);
    if (!conn) {
        graphics_write_textr("wget: connection failed\n");
        return;
    }

    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: MinimalOS-wget\r\n\r\n",
        path, host);

    if (tcp_send(conn, request, (uint32_t)req_len, 5000) != req_len) {
        graphics_write_textr("wget: failed to send request\n");
        tcp_close(conn);
        return;
    }

    graphics_write_textr("Downloading...\n");

    minimafs_file_handle_t* out_file = NULL;
    char resolved_path[MINIMAFS_MAX_PATH];
    if (save_path) {
        if (!fs_resolve_path(save_path, resolved_path)) {
            graphics_write_textr("wget: invalid output path\n");
            tcp_close(conn);
            return;
        }
        minimafs_file_handle_t* existing = minimafs_open(resolved_path, true);
        if (existing) { minimafs_close(existing); minimafs_delete_file(resolved_path); }
        if (!minimafs_create_file(resolved_path, "binary", "bin")) {
            graphics_write_textr("wget: could not create output file\n");
            tcp_close(conn);
            return;
        }
        out_file = minimafs_open(resolved_path, false);
        if (!out_file) {
            graphics_write_textr("wget: could not open output file\n");
            tcp_close(conn);
            return;
        }
    }

    bool headers_done = false;
    int header_match = 0;
    static uint8_t chunk[1024];
    uint64_t total_bytes = 0;
    const uint64_t IDLE_TIMEOUT_MS = 8000;
    uint64_t last_data_ms = time_get_uptime_ms();

    while (true) {
        int32_t got = tcp_recv(conn, chunk, sizeof(chunk));
        if (got < 0) break;
        if (got == 0) {
            if (time_get_uptime_ms() - last_data_ms > IDLE_TIMEOUT_MS) break;
            tcp_poll();
            sleep(5);
            continue;
        }
        last_data_ms = time_get_uptime_ms();

        uint32_t body_start = 0;
        if (!headers_done) {
            uint32_t i = 0;
            for (; i < (uint32_t)got; i++) {
                char c = (char)chunk[i];
                if ((header_match == 0 || header_match == 2) && c == '\r') header_match++;
                else if ((header_match == 1 || header_match == 3) && c == '\n') header_match++;
                else header_match = (c == '\r') ? 1 : 0;
                if (header_match == 4) { headers_done = true; body_start = i + 1; break; }
            }
            if (!headers_done) continue;
        }

        uint32_t body_len = (uint32_t)got - body_start;
        if (body_len > 0) {
            if (out_file) {
                minimafs_write(out_file, chunk + body_start, body_len);
            } else {
                for (uint32_t i = 0; i < body_len; i++) {
                    graphics_write_textr_char((char)chunk[body_start + i]);
                }
            }
            total_bytes += body_len;
        }
    }

    if (out_file) minimafs_close(out_file);
    tcp_close(conn);

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
    command_register("ifconfig", cmd_ifconfig);
    command_register("dhcp", cmd_dhcp);
    command_register("wget", cmd_wget);
}

REGISTER_COMMAND(register_net_commands);
