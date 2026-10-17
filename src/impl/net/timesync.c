#include <stdint.h>
#include <stdbool.h>
#include "net/timesync.h"
#include "net/dns.h"
#include "net/ip.h"
#include "net/tcp.h"
#include "net/tls13.h"
#include "x86_64/net_syscall.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/spinlock.h"
#include "serial.h"
#include "string.h"
#include "time.h"

/*
 * Internet time.
 *
 * SYS_TIMESYNC stores a configuration (address, polling interval, on/off);
 * the first time it is switched on a kernel process is started that, every
 * interval, fetches the address and reads the first number in the reply as
 * Unix milliseconds. A plausible value sets the system clock in UTC.
 *
 * The answer must be in milliseconds: a 10-digit seconds value is rejected
 * rather than guessed at, so a wrong address shows up as an error instead
 * of a clock that is off by a factor of 1000.
 */

#define TIMESYNC_CONNECT_TIMEOUT_MS 10000u
#define TIMESYNC_IDLE_TIMEOUT_MS    6000u
#define TIMESYNC_RESPONSE_MAX       4096u
// 2001-09-09 .. 2286-11-20: every 13-digit millisecond time.
#define TIMESYNC_MIN_UNIX_MS 1000000000000ull
#define TIMESYNC_MAX_UNIX_MS 9999999999999ull

typedef struct {
    bool enabled;
    uint32_t interval_s;
    char url[SYS_TIMESYNC_URL_SIZE];
    uint32_t generation;        // bumped by every CONFIGURE
} timesync_config_t;

typedef struct {
    bool synced;
    uint64_t last_unix_ms;
    uint64_t last_sync_uptime_ms;
    uint64_t attempts;
    char error[SYS_TIMESYNC_ERROR_SIZE];
} timesync_status_t;

static spinlock_t g_timesync_lock = SPINLOCK_INIT;
static timesync_config_t g_config = {false, 30u, "", 0u};
static timesync_status_t g_status;
static volatile bool g_worker_started;

static void set_error(const char* message) {
    uint64_t flags = spinlock_acquire(&g_timesync_lock);
    strncpy(g_status.error, message, sizeof(g_status.error) - 1u);
    g_status.error[sizeof(g_status.error) - 1u] = '\0';
    spinlock_release(&g_timesync_lock, flags);
}

void timesync_unix_ms_to_datetime(uint64_t unix_ms, uint16_t* year,
                                  uint8_t* month, uint8_t* day,
                                  uint8_t* hour, uint8_t* minute,
                                  uint8_t* second) {
    uint64_t seconds = unix_ms / 1000u;
    uint64_t days = seconds / 86400u;
    uint64_t rest = seconds % 86400u;
    *hour = (uint8_t)(rest / 3600u);
    *minute = (uint8_t)((rest % 3600u) / 60u);
    *second = (uint8_t)(rest % 60u);
    // Howard Hinnant's civil_from_days, for days since 1970-01-01.
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint64_t mp = (5u * doy + 2u) / 153u;
    uint64_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint64_t m = mp < 10u ? mp + 3u : mp - 9u;
    if (m <= 2u) y++;
    *year = (uint16_t)y;
    *month = (uint8_t)m;
    *day = (uint8_t)d;
}

static bool parse_url(const char* url, char* host, size_t host_size,
                      uint16_t* port, char* path, size_t path_size,
                      bool* https) {
    const char* p = url;
    *https = false;
    if (strncmp(p, "https://", 8) == 0) { *https = true; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return false;

    size_t length = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?') {
        if (length + 1u >= host_size) return false;
        host[length++] = *p++;
    }
    host[length] = '\0';
    if (!length) return false;

    *port = *https ? 443u : 80u;
    if (*p == ':') {
        p++;
        uint32_t value = 0;
        while (*p >= '0' && *p <= '9' && value <= 65535u)
            value = value * 10u + (uint32_t)(*p++ - '0');
        if (!value || value > 65535u) return false;
        *port = (uint16_t)value;
    }

    if (*p == '/' || *p == '?') {
        size_t i = 0;
        if (*p == '?') path[i++] = '/';
        while (*p && i + 1u < path_size) path[i++] = *p++;
        if (*p) return false;
        path[i] = '\0';
    } else {
        strcpy(path, "/");
    }
    return true;
}

// The first 13-digit number in the body ({"unix_timestamp":1792259743407}),
// as Unix milliseconds. Shorter runs (a chunk length, seconds) are skipped;
// "1729170000.123" style seconds with a fraction never qualify.
static bool parse_unix_ms(const char* body, uint32_t length, uint64_t* out) {
    for (uint32_t i = 0; i < length; i++) {
        if (body[i] < '0' || body[i] > '9') continue;
        uint64_t value = 0;
        uint32_t digits = 0;
        while (i < length && body[i] >= '0' && body[i] <= '9') {
            if (digits++ < 19u) value = value * 10u + (uint64_t)(body[i] - '0');
            i++;
        }
        bool fraction = i < length && body[i] == '.';
        if (digits == 13u && !fraction) {
            *out = value;
            return true;
        }
        while (fraction && ++i < length && body[i] >= '0' && body[i] <= '9') {}
    }
    return false;
}

// One request. Returns true and the time on success; sets the error text
// otherwise. The network stack is claimed only around each call into it.
static bool fetch_unix_ms(const char* url, uint64_t* unix_ms) {
    char host[128];
    char path[SYS_TIMESYNC_URL_SIZE];
    uint16_t port;
    bool https;
    if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path),
                   &https)) {
        set_error("Address must start with http:// or https://");
        return false;
    }
    if (!ip_is_configured()) {
        set_error("No network address yet");
        return false;
    }

    uint32_t ip = ip_parse(host);
    if (!ip) {
        if (!net_stack_claim()) { set_error("Network busy"); return false; }
        bool resolved = dns_resolve(host, &ip, 5000u);
        net_stack_release();
        if (!resolved) { set_error("Could not resolve the host"); return false; }
    }

    if (!net_stack_claim()) { set_error("Network busy"); return false; }
    tcp_conn_t* conn = NULL;
    tls13_client_t* tls = NULL;
    if (https) tls = tls13_connect(ip, port, host, TIMESYNC_CONNECT_TIMEOUT_MS);
    else conn = tcp_connect(ip, port, TIMESYNC_CONNECT_TIMEOUT_MS);
    net_stack_release();
    if (https ? !tls : !conn) {
        set_error(https ? "TLS connection failed" : "Connection failed");
        return false;
    }

    static char request[SYS_TIMESYNC_URL_SIZE + 192u];
    int request_length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s\r\nAccept: */*\r\n"
        "Connection: close\r\nUser-Agent: MinimalOS-timesync\r\n\r\n",
        path, host);

    bool ok = false;
    if (!net_stack_claim()) {
        set_error("Network busy");
        goto close;
    }
    int32_t sent = https ? tls13_send(tls, request, (uint32_t)request_length)
                         : tcp_send(conn, request, (uint32_t)request_length,
                                    5000u);
    net_stack_release();
    if (sent != request_length) {
        set_error("Could not send the request");
        goto close;
    }

    static char response[TIMESYNC_RESPONSE_MAX + 1u];
    uint32_t used = 0;
    uint64_t last_data = time_get_uptime_ms();
    for (;;) {
        if (used >= TIMESYNC_RESPONSE_MAX) break;
        if (!net_stack_claim()) { sleep(10); continue; }
        // tls13_recv() blocks for a whole record (a timeout mid-record
        // would lose it) and returns -1 on timeout or close; plain TCP is
        // polled.
        int32_t got = https
            ? tls13_recv(tls, response + used, TIMESYNC_RESPONSE_MAX - used,
                         TIMESYNC_IDLE_TIMEOUT_MS)
            : tcp_recv(conn, response + used, TIMESYNC_RESPONSE_MAX - used);
        if (got == 0 && !https) tcp_poll();
        net_stack_release();
        if (got < 0) break;             // closed: the answer is complete
        if (got > 0) {
            used += (uint32_t)got;
            last_data = time_get_uptime_ms();
            continue;
        }
        if (time_get_uptime_ms() - last_data > TIMESYNC_IDLE_TIMEOUT_MS) break;
        sleep(10);
    }
    response[used] = '\0';

    // "HTTP/1.x 200 ..." then headers, a blank line and the body.
    if (used < 12u || strncmp(response, "HTTP/1.", 7) != 0) {
        set_error(used ? "Not an HTTP answer" : "No answer from the server");
        goto close;
    }
    if (strncmp(response + 9, "200", 3) != 0) {
        char message[SYS_TIMESYNC_ERROR_SIZE];
        snprintf(message, sizeof(message), "Server answered HTTP %c%c%c",
                 response[9], response[10], response[11]);
        set_error(message);
        goto close;
    }
    uint32_t body = 0;
    for (uint32_t i = 3; i < used; i++) {
        if (response[i - 3] == '\r' && response[i - 2] == '\n' &&
            response[i - 1] == '\r' && response[i] == '\n') {
            body = i + 1u;
            break;
        }
    }
    if (!body) {
        set_error("Incomplete HTTP answer");
        goto close;
    }
    uint64_t value = 0;
    if (!parse_unix_ms(response + body, used - body, &value) ||
        value < TIMESYNC_MIN_UNIX_MS || value > TIMESYNC_MAX_UNIX_MS) {
        set_error("Answer is not Unix time in milliseconds");
        goto close;
    }
    *unix_ms = value;
    ok = true;

close:
    while (!net_stack_claim()) sleep(10);
    if (https) tls13_close(tls); else tcp_close(conn);
    net_stack_release();
    return ok;
}

static void timesync_worker(void) {
    uint32_t synced_generation = UINT32_MAX;
    uint64_t next_due = 0;
    for (;;) {
        uint64_t flags = spinlock_acquire(&g_timesync_lock);
        timesync_config_t config = g_config;
        spinlock_release(&g_timesync_lock, flags);

        uint64_t now = time_get_uptime_ms();
        // A new configuration is tried straight away.
        if (config.enabled &&
            (config.generation != synced_generation || now >= next_due)) {
            synced_generation = config.generation;
            next_due = now + (uint64_t)config.interval_s * 1000u;

            flags = spinlock_acquire(&g_timesync_lock);
            g_status.attempts++;
            spinlock_release(&g_timesync_lock, flags);

            uint64_t unix_ms = 0;
            if (fetch_unix_ms(config.url, &unix_ms)) {
                datetime_t dt;
                timesync_unix_ms_to_datetime(unix_ms, &dt.year, &dt.month,
                    &dt.day, &dt.hour, &dt.minute, &dt.second);
                time_set_datetime(&dt);
                flags = spinlock_acquire(&g_timesync_lock);
                g_status.synced = true;
                g_status.last_unix_ms = unix_ms;
                g_status.last_sync_uptime_ms = time_get_uptime_ms();
                g_status.error[0] = '\0';
                spinlock_release(&g_timesync_lock, flags);
                char line[64];
                snprintf(line, sizeof(line),
                         "timesync: synced %04u-%02u-%02u %02u:%02u:%02u UTC\n",
                         dt.year, dt.month, dt.day, dt.hour, dt.minute,
                         dt.second);
                serial_write_str(line);
            } else {
                serial_write_str("timesync: ");
                serial_write_str(g_status.error);
                serial_write_str("\n");
            }
        }
        sleep(250);
    }
}

uint64_t timesync_syscall(syscall_timesync_t* request) {
    if (!request) return SYS_ERR_INVAL;
    if (request->op == SYS_TIMESYNC_CONFIGURE) {
        uint32_t interval = request->interval_s;
        if (interval < SYS_TIMESYNC_MIN_INTERVAL ||
            interval > SYS_TIMESYNC_MAX_INTERVAL)
            return SYS_ERR_INVAL;
        if (request->url[SYS_TIMESYNC_URL_SIZE - 1u] != '\0')
            return SYS_ERR_INVAL;
        bool start = false;
        uint64_t flags = spinlock_acquire(&g_timesync_lock);
        bool changed = g_config.enabled != (request->enabled != 0u) ||
                       g_config.interval_s != interval ||
                       strcmp(g_config.url, request->url) != 0;
        if (changed) {
            g_config.enabled = request->enabled != 0u;
            g_config.interval_s = interval;
            strcpy(g_config.url, request->url);
            g_config.generation++;
            if (!g_config.enabled) g_status.error[0] = '\0';
        }
        if (g_config.enabled && !g_worker_started) {
            g_worker_started = true;
            start = true;
        }
        spinlock_release(&g_timesync_lock, flags);
        if (start && !proc_create("timesync", timesync_worker)) {
            g_worker_started = false;
            set_error("Could not start the time service");
            return SYS_ERR_GENERIC;
        }
        return SYS_SUCCESS;
    }
    if (request->op == SYS_TIMESYNC_STATUS) {
        uint64_t flags = spinlock_acquire(&g_timesync_lock);
        request->enabled = g_config.enabled ? 1u : 0u;
        request->interval_s = g_config.interval_s;
        strcpy(request->url, g_config.url);
        request->synced = g_status.synced ? 1u : 0u;
        request->last_unix_ms = g_status.last_unix_ms;
        request->last_sync_age_ms = g_status.synced
            ? time_get_uptime_ms() - g_status.last_sync_uptime_ms : 0u;
        request->attempts = g_status.attempts;
        strcpy(request->error, g_status.error);
        spinlock_release(&g_timesync_lock, flags);
        return SYS_SUCCESS;
    }
    return SYS_ERR_INVAL;
}
