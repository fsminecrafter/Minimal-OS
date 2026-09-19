#include "x86_64/random.h"
#include "x86_64/rtc.h"
#include "x86_64/spinlock.h"
#include "time.h"
#include "string.h"
#include "serial.h"

static spinlock_t g_random_lock = SPINLOCK_INIT;
static uint64_t g_state;
static bool g_has_rdrand = false;
static bool g_initialized = false;

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1), "c"(0));
    return (ecx & (1u << 30)) != 0;   // CPUID.01H:ECX.RDRAND[bit 30]
}

// RDRAND can legitimately fail (entropy pool drained); the ISA
// recommends retrying a bounded number of times and then falling back.
static bool rdrand64(uint64_t* out) {
    for (int attempt = 0; attempt < 10; attempt++) {
        uint64_t value;
        uint8_t ok;
        asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) { *out = value; return true; }
    }
    return false;
}

// SplitMix64 - small, fast, good avalanche. Not a stream cipher; it is
// the mixing function over a pool that gets reseeded, which is enough
// for the threat model here (a LAN peer guessing a session key).
static uint64_t splitmix64(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void random_init(void) {
    uint64_t flags = spinlock_acquire(&g_random_lock);

    g_has_rdrand = cpu_has_rdrand();

    uint64_t seed = 0x243F6A8885A308D3ull;
    seed ^= read_tsc();
    seed ^= (uint64_t)time_get_uptime_ms() << 17;

    datetime_t dt;
    rtc_get_datetime(&dt);
    seed ^= ((uint64_t)dt.year << 40) ^ ((uint64_t)dt.month << 32) ^
            ((uint64_t)dt.day << 24) ^ ((uint64_t)dt.hour << 16) ^
            ((uint64_t)dt.minute << 8) ^ (uint64_t)dt.second;

    // TSC jitter: the low bits of successive reads are genuinely noisy
    // on real hardware and at least non-trivial under emulation.
    for (int i = 0; i < 64; i++) {
        seed = (seed << 1) | (read_tsc() & 1ull);
        seed ^= read_tsc() >> 3;
    }

    if (g_has_rdrand) {
        uint64_t hw;
        if (rdrand64(&hw)) seed ^= hw;
    }

    g_state = seed;
    g_initialized = true;
    spinlock_release(&g_random_lock, flags);

    serial_write_str(g_has_rdrand ? "[random] seeded (RDRAND present)\n"
                                  : "[random] seeded (no RDRAND - TSC/RTC fallback)\n");
}

bool random_has_hardware(void) {
    return g_has_rdrand;
}

void random_add_entropy(uint64_t value) {
    uint64_t flags = spinlock_acquire(&g_random_lock);
    g_state ^= value * 0x9E3779B97F4A7C15ull;
    g_state ^= read_tsc();
    spinlock_release(&g_random_lock, flags);
}

uint64_t random_u64(void) {
    if (!g_initialized) random_init();

    uint64_t flags = spinlock_acquire(&g_random_lock);
    g_state ^= read_tsc();
    uint64_t out = splitmix64(&g_state);
    if (g_has_rdrand) {
        uint64_t hw;
        if (rdrand64(&hw)) out ^= hw;
    }
    spinlock_release(&g_random_lock, flags);
    return out;
}

void random_bytes(void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    size_t produced = 0;
    while (produced < len) {
        uint64_t chunk = random_u64();
        size_t n = len - produced;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        memcpy(p + produced, &chunk, n);
        produced += n;
    }
}
