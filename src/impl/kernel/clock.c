#include "x86_64/clock.h"
#include "serial.h"
#include "time.h"
#include "timeconv.h"
#include <stdbool.h>

static bool     g_tsc_ok = false;
static bool     g_tsc_invariant = false;
static uint64_t g_tsc_hz = 0;
static uint64_t g_ns_per_tick_q32 = 0;
static uint64_t g_anchor_tsc = 0;
static uint64_t g_anchor_ns = 0;
static volatile uint64_t g_last_ns = 0;

static inline void cpuid_(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b,
                          uint32_t* c, uint32_t* d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(sub));
}

static inline uint64_t rdtsc_(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Measures TSC Hz against the 1 kHz uptime counter. 0 on failure.
static uint64_t measure_tsc_hz(void) {
    const uint32_t MAX_ITER = 200000000u;
    uint32_t iter = 0;

    // Align to a millisecond tick edge.
    uint64_t t0 = time_get_uptime_ms();
    while (time_get_uptime_ms() == t0) {
        asm volatile("pause" ::: "memory");
        if (++iter > MAX_ITER) return 0;
    }
    uint64_t start_ms = time_get_uptime_ms();
    uint64_t start_tsc = rdtsc_();

    uint64_t deadline = start_ms + 100;
    iter = 0;
    while (time_get_uptime_ms() < deadline) {
        asm volatile("pause" ::: "memory");
        if (++iter > MAX_ITER) break;
    }
    uint64_t end_ms = time_get_uptime_ms();
    uint64_t end_tsc = rdtsc_();

    uint64_t ms = end_ms - start_ms;
    if (ms == 0 || end_tsc <= start_tsc) return 0;
    return (end_tsc - start_tsc) * 1000ULL / ms;
}

void clock_init(void) {
    uint32_t a, b, c, d;
    cpuid_(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 4))) {
        serial_write_str("clock: no TSC, using uptime ms\n");
        return;
    }

    bool hypervisor = (c & (1u << 31)) != 0;      // CPUID.1:ECX, running in a VM

    uint32_t max_basic, max_ext;
    cpuid_(0, 0, &max_basic, &b, &c, &d);
    cpuid_(0x80000000u, 0, &max_ext, &b, &c, &d);
    bool invariant = false;
    if (max_ext >= 0x80000007u) {
        cpuid_(0x80000007u, 0, &a, &b, &c, &d);
        invariant = (d & (1u << 8)) != 0;
    }

    // A TSC that changes speed with the CPU's power state (old hardware)
    // would drift away from the millisecond clock that sleeps and timeouts
    // use. Virtual machines keep it steady without advertising the bit.
    if (!invariant && !hypervisor) {
        serial_write_str("clock: TSC is not invariant, using uptime ms\n");
        return;
    }

    uint64_t hz = 0;
    const char* src = "measured";
    if (max_basic >= 0x15) {
        uint32_t ea, eb, ec;
        cpuid_(0x15, 0, &ea, &eb, &ec, &d);
        if (ea && eb && ec) {
            hz = (uint64_t)ec * eb / ea;
            src = "cpuid15";
        }
    }
    if (hz == 0) {
        hz = measure_tsc_hz();
        src = "measured";
    }

    if (hz == 0) {
        serial_write_str("clock: TSC calibration failed, using uptime ms\n");
        return;
    }

    g_ns_per_tick_q32 = timeconv_ns_per_tick_q32(hz);
    if (g_ns_per_tick_q32 == 0) return;
    g_anchor_ns = time_get_uptime_ms() * 1000000ULL;
    g_anchor_tsc = rdtsc_();
    g_last_ns = g_anchor_ns;
    g_tsc_hz = hz;
    g_tsc_invariant = invariant;
    __sync_synchronize();
    g_tsc_ok = true;

    serial_write_str("clock: TSC ");
    serial_write_dec(hz);
    serial_write_str(" Hz (");
    serial_write_str(src);
    serial_write_str("), invariant=");
    serial_write_str(invariant ? "yes" : "no");
    serial_write_str("\n");
}

uint64_t clock_monotonic_ns(void) {
    uint64_t ns;
    if (!g_tsc_ok) {
        ns = time_get_uptime_ms() * 1000000ULL;
    } else {
        uint64_t tsc = rdtsc_();
        uint64_t delta = tsc > g_anchor_tsc ? tsc - g_anchor_tsc : 0;
        ns = g_anchor_ns + timeconv_ticks_to_ns(delta, g_ns_per_tick_q32);
    }

    // Monotonic clamp across CPUs: atomic max.
    uint64_t last = g_last_ns;
    while (ns > last) {
        uint64_t seen = __sync_val_compare_and_swap(&g_last_ns, last, ns);
        if (seen == last) return ns;
        last = seen;
    }
    return last;
}

uint64_t clock_realtime_ns(void) {
    datetime_t dt;
    uint32_t sub;
    uint64_t up;
    int tries = 0;
    do {
        up = time_get_uptime_ms();
        dt = time_get_datetime();
        sub = time_get_subsecond_ms();
    } while (up != time_get_uptime_ms() && ++tries < 8);

    int64_t secs = timeconv_unix_from_civil(dt.year, dt.month, dt.day,
                                            dt.hour, dt.minute, dt.second);
    if (secs < 0) secs = 0;
    return (uint64_t)secs * 1000000000ULL + (uint64_t)sub * 1000000ULL;
}

uint64_t clock_tsc_hz(void) {
    return (g_tsc_ok && g_tsc_invariant) ? g_tsc_hz : 0;
}
