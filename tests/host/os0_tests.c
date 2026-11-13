// Host tests for timeconv.c, fspaths.c and argblock.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "timeconv.h"
#include "fspaths.h"
#include "x86_64/loader/argblock.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ_I(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; \
    if (_a != _b) { g_failures++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
    __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { const char* _a = (a); const char* _b = (b); g_checks++; \
    if (!_a || strcmp(_a, _b) != 0) { g_failures++; printf("FAIL %s:%d: \"%s\" vs \"%s\"\n", \
    __FILE__, __LINE__, _a ? _a : "(null)", _b); } } while (0)

// ---- stub for the kernel's MinimaFS path parser ----
bool minimafs_parse_path(const char* path, uint8_t* drive_number, char* local_path) {
    if (!path || !drive_number || !local_path) return false;
    const char* colon = strchr(path, ':');
    if (!colon) return false;
    size_t nlen = (size_t)(colon - path);
    if (nlen == 0) return false;
    bool numeric = true;
    for (size_t i = 0; i < nlen; i++) if (path[i] < '0' || path[i] > '9') numeric = false;
    if (numeric) {
        if (nlen > 2) return false;
        int n = 0;
        for (size_t i = 0; i < nlen; i++) n = n * 10 + (path[i] - '0');
        *drive_number = (uint8_t)n;
    } else if (nlen == 4 && strncmp(path, "data", 4) == 0) {
        *drive_number = 5;
    } else {
        return false;
    }
    const char* rest = colon + 1;
    if (!*rest) strcpy(local_path, "/");
    else strcpy(local_path, rest);
    return true;
}

static void test_civil(void) {
    CHECK_EQ_I(timeconv_unix_from_civil(1970, 1, 1, 0, 0, 0), 0);
    CHECK_EQ_I(timeconv_unix_from_civil(1970, 1, 1, 1, 2, 3), 3723);
    CHECK_EQ_I(timeconv_unix_from_civil(2000, 2, 29, 0, 0, 0), 951782400LL);
    CHECK_EQ_I(timeconv_unix_from_civil(2000, 3, 1, 0, 0, 0), 951868800LL);
    CHECK_EQ_I(timeconv_unix_from_civil(1900, 3, 1, 0, 0, 0), -2203891200LL);
    CHECK_EQ_I(timeconv_unix_from_civil(2026, 11, 12, 0, 0, 0), 1794441600LL);
    CHECK_EQ_I(timeconv_unix_from_civil(2038, 1, 19, 3, 14, 7), 2147483647LL);
    CHECK_EQ_I(timeconv_unix_from_civil(1969, 12, 31, 23, 59, 59), -1);
    // Leap-year edges: 2000 leap, 1900 not, 2024 leap, 2023 not, 2100 not.
    CHECK_EQ_I(timeconv_unix_from_civil(2000, 3, 1, 0, 0, 0) - timeconv_unix_from_civil(2000, 2, 28, 0, 0, 0), 2 * 86400);
    CHECK_EQ_I(timeconv_unix_from_civil(1900, 3, 1, 0, 0, 0) - timeconv_unix_from_civil(1900, 2, 28, 0, 0, 0), 86400);
    CHECK_EQ_I(timeconv_unix_from_civil(2024, 3, 1, 0, 0, 0) - timeconv_unix_from_civil(2024, 2, 28, 0, 0, 0), 2 * 86400);
    CHECK_EQ_I(timeconv_unix_from_civil(2023, 3, 1, 0, 0, 0) - timeconv_unix_from_civil(2023, 2, 28, 0, 0, 0), 86400);
    CHECK_EQ_I(timeconv_unix_from_civil(2100, 3, 1, 0, 0, 0) - timeconv_unix_from_civil(2100, 2, 28, 0, 0, 0), 86400);
    // Year lengths.
    CHECK_EQ_I(timeconv_unix_from_civil(2001, 1, 1, 0, 0, 0) - timeconv_unix_from_civil(2000, 1, 1, 0, 0, 0), 366LL * 86400);
    CHECK_EQ_I(timeconv_unix_from_civil(1901, 1, 1, 0, 0, 0) - timeconv_unix_from_civil(1900, 1, 1, 0, 0, 0), 365LL * 86400);
    // Day-by-day across a leap year.
    int64_t prev = timeconv_unix_from_civil(1999, 12, 31, 0, 0, 0);
    static const int mdays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m <= 12; m++)
        for (int d = 1; d <= mdays[m - 1]; d++) {
            int64_t t = timeconv_unix_from_civil(2000, m, d, 0, 0, 0);
            CHECK_EQ_I(t - prev, 86400);
            prev = t;
        }
    CHECK_EQ_I(timeconv_unix_from_civil(2001, 1, 1, 0, 0, 0) - prev, 86400);
}

static void test_date_parse(void) {
    int64_t s = 12345;
    CHECK(timeconv_parse_minimafs_date("12Nov2026", &s));
    CHECK_EQ_I(s, 1794441600LL);
    CHECK(timeconv_parse_minimafs_date("01Jan1970", &s));
    CHECK_EQ_I(s, 0);
    CHECK(timeconv_parse_minimafs_date("29Feb2000", &s));
    CHECK_EQ_I(s, 951782400LL);
    CHECK(timeconv_parse_minimafs_date("12NOV2026", &s));
    CHECK_EQ_I(s, 1794441600LL);
    CHECK(timeconv_parse_minimafs_date("12nov2026", &s));
    CHECK_EQ_I(s, 1794441600LL);
    CHECK(timeconv_parse_minimafs_date("01mAr1900", &s));
    CHECK_EQ_I(s, -2203891200LL);
    static const char* names[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int m = 0; m < 12; m++) {
        char buf[16];
        snprintf(buf, sizeof buf, "15%s2001", names[m]);
        CHECK(timeconv_parse_minimafs_date(buf, &s));
        CHECK_EQ_I(s, timeconv_unix_from_civil(2001, m + 1, 15, 0, 0, 0));
    }
    // Rejections; the output stays untouched.
    s = 777;
    CHECK(!timeconv_parse_minimafs_date("1Nov2026", &s));
    CHECK(!timeconv_parse_minimafs_date("12Foo2026", &s));
    CHECK(!timeconv_parse_minimafs_date("12Nov20x6", &s));
    CHECK(!timeconv_parse_minimafs_date("12Nov202", &s));
    CHECK(!timeconv_parse_minimafs_date("", &s));
    CHECK(!timeconv_parse_minimafs_date("Nov122026", &s));
    CHECK(!timeconv_parse_minimafs_date("00Nov2026", &s));
    CHECK(!timeconv_parse_minimafs_date("32Nov2026", &s));
    CHECK(!timeconv_parse_minimafs_date("1", &s));
    CHECK(!timeconv_parse_minimafs_date(NULL, &s));
    CHECK(!timeconv_parse_minimafs_date("12Nov2026", NULL));
    CHECK_EQ_I(s, 777);
}

static void test_ticks(void) {
    static const uint64_t hzs[] = {1000000000ULL, 2800000000ULL, 3000000000ULL, 32768ULL, 1ULL, 2400000000ULL};
    CHECK_EQ_I(timeconv_ns_per_tick_q32(0), 0);
    CHECK_EQ_I(timeconv_ticks_to_ns(12345, 0), 0);
    CHECK(timeconv_ns_per_tick_q32(1000000000ULL) == (1ULL << 32));
    CHECK(timeconv_ticks_to_ns(123456789ULL, timeconv_ns_per_tick_q32(1000000000ULL)) == 123456789ULL);
    for (size_t h = 0; h < sizeof hzs / sizeof hzs[0]; h++) {
        uint64_t hz = hzs[h];
        uint64_t q = timeconv_ns_per_tick_q32(hz);
        // One second of ticks is one second of ns, within the truncation
        // error of the factor (under one q32 ulp per tick, so ticks / 2^32 ns).
        uint64_t ns = timeconv_ticks_to_ns(hz, q);
        CHECK(ns <= 1000000000ULL);
        CHECK(1000000000ULL - ns <= hz / 4294967296ULL + 1);
        // Large counts against the exact ticks * 1e9 / hz.
        static const uint64_t ticks[] = {1, 1000, 1000000ULL, 1000000000000ULL, 1000000000000000ULL, 10000000000000000ULL};
        uint64_t last = 0;
        for (size_t i = 0; i < sizeof ticks / sizeof ticks[0]; i++) {
            unsigned __int128 exact = (unsigned __int128)ticks[i] * 1000000000u / hz;
            if (exact > UINT64_MAX) continue;                  // result does not fit in 64 bits
            uint64_t got = timeconv_ticks_to_ns(ticks[i], q);
            uint64_t ex = (uint64_t)exact;
            uint64_t diff = ex > got ? ex - got : got - ex;
            CHECK(got <= ex);                                  // truncation never overshoots
            CHECK(diff <= ticks[i] / 4294967296ULL + 1);
            CHECK(got >= last);
            last = got;
        }
        // Monotonic over a dense sweep around a large base.
        uint64_t base = 1000000000000000ULL, prev = timeconv_ticks_to_ns(base, q);
        unsigned __int128 top = (unsigned __int128)(base + 5000 * 7919ULL) * 1000000000u / hz;
        for (uint64_t k = 1; k < 5000 && top <= UINT64_MAX; k++) {
            uint64_t cur = timeconv_ticks_to_ns(base + k * 7919, q);
            CHECK(cur >= prev);
            prev = cur;
        }
    }
    CHECK(timeconv_ticks_to_ns(1000000000000000ULL, timeconv_ns_per_tick_q32(1000000000ULL)) == 1000000000000000ULL);
    uint64_t n3 = timeconv_ticks_to_ns(1000000000000000ULL, timeconv_ns_per_tick_q32(3000000000ULL));
    CHECK(n3 >= 333333333333333ULL - 300000 && n3 <= 333333333333333ULL);
    // The full tick range at 1 GHz does not overflow the 128-bit intermediate.
    CHECK(timeconv_ticks_to_ns(UINT64_MAX, timeconv_ns_per_tick_q32(1000000000ULL)) == UINT64_MAX);
}

static void test_paths(void) {
    char out[MINIMAFS_MAX_PATH];
    memset(out, 'x', sizeof out);

    CHECK(fs_resolve_path_from("0:/a/b", "c", out));            CHECK_STR(out, "0:/a/b/c");
    CHECK(fs_resolve_path_from("0:/a/b", "", out));             CHECK_STR(out, "0:/a/b");
    CHECK(fs_resolve_path_from("0:/", "", out));                CHECK_STR(out, "0:/");
    CHECK(fs_resolve_path_from("0:", "", out));                 CHECK_STR(out, "0:/");
    CHECK(fs_resolve_path_from("3:/a/b", "/x", out));           CHECK_STR(out, "3:/x");
    CHECK(fs_resolve_path_from("3:/a/b", "/", out));            CHECK_STR(out, "3:/");
    CHECK(fs_resolve_path_from("0:/a/b", "1:/z/y", out));       CHECK_STR(out, "1:/z/y");
    CHECK(fs_resolve_path_from("0:/a/b", "1:", out));           CHECK_STR(out, "1:/");
    CHECK(fs_resolve_path_from("0:/a/b", "data:/q", out));      CHECK_STR(out, "5:/q");
    CHECK(fs_resolve_path_from("0:/a/b", ".", out));            CHECK_STR(out, "0:/a/b");
    CHECK(fs_resolve_path_from("0:/a/b", "./c/./d", out));      CHECK_STR(out, "0:/a/b/c/d");
    CHECK(fs_resolve_path_from("0:/a/b", "..", out));           CHECK_STR(out, "0:/a");
    CHECK(fs_resolve_path_from("0:/a/b", "../..", out));        CHECK_STR(out, "0:/");
    CHECK(fs_resolve_path_from("0:/a/b", "../../..", out));     CHECK_STR(out, "0:/");
    CHECK(fs_resolve_path_from("0:/a/b", "../../../../x", out)); CHECK_STR(out, "0:/x");
    CHECK(fs_resolve_path_from("0:/a/b", "../c", out));         CHECK_STR(out, "0:/a/c");
    CHECK(fs_resolve_path_from("0:/a/b", "c/../d", out));       CHECK_STR(out, "0:/a/b/d");
    CHECK(fs_resolve_path_from("0:/a/b", "//x///y//", out));    CHECK_STR(out, "0:/x/y");
    CHECK(fs_resolve_path_from("0:/a//b/", "c//d", out));       CHECK_STR(out, "0:/a/b/c/d");
    CHECK(fs_resolve_path_from("0:/a/b", "0:/../..", out));     CHECK_STR(out, "0:/");
    CHECK(fs_resolve_path_from("12:/a", "b", out));             CHECK_STR(out, "12:/a/b");
    CHECK(fs_resolve_path_from("99:/", "x", out));              CHECK_STR(out, "99:/x");
    CHECK(fs_resolve_path_from("0:/a", "..hidden", out));       CHECK_STR(out, "0:/a/..hidden");
    CHECK(fs_resolve_path_from("0:/a", "...", out));            CHECK_STR(out, "0:/a/...");
    CHECK(fs_resolve_path_from("0:/a", "x/", out));             CHECK_STR(out, "0:/a/x");
    // Failures.
    CHECK(!fs_resolve_path_from("0:/a", "nope:/x", out));
    CHECK(!fs_resolve_path_from("nope:/a", "x", out));
    CHECK(!fs_resolve_path_from(NULL, "x", out));
    CHECK(!fs_resolve_path_from("0:/a", NULL, out));
    CHECK(!fs_resolve_path_from("0:/a", "x", NULL));

    // A path longer than MINIMAFS_MAX_PATH is rejected.
    char longp[MINIMAFS_MAX_PATH * 3];
    size_t n = 0;
    longp[n++] = '/';
    while (n < MINIMAFS_MAX_PATH + 200) { longp[n++] = 'a'; if (n % 50 == 0) longp[n++] = '/'; }
    longp[n] = '\0';
    CHECK(!fs_resolve_path_from("0:/", longp, out));
    // Accumulating relative segments until it no longer fits.
    char base[MINIMAFS_MAX_PATH];
    strcpy(base, "0:/");
    bool failed = false;
    for (int i = 0; i < 40 && !failed; i++) {
        char next[MINIMAFS_MAX_PATH];
        char seg[64];
        memset(seg, 'd', 63); seg[63] = '\0';
        if (!fs_resolve_path_from(base, seg, next)) failed = true;
        else {
            CHECK(strlen(next) < MINIMAFS_MAX_PATH);
            strcpy(base, next);
        }
    }
    CHECK(failed);
    // A component of MINIMAFS_MAX_FILENAME or more is rejected, one shorter is fine.
    char comp[MINIMAFS_MAX_FILENAME + 1];
    memset(comp, 'f', sizeof comp); comp[MINIMAFS_MAX_FILENAME] = '\0';
    CHECK(!fs_resolve_path_from("0:/", comp, out));
    comp[MINIMAFS_MAX_FILENAME - 1] = '\0';
    CHECK(fs_resolve_path_from("0:/", comp, out));
    CHECK(strlen(out) < MINIMAFS_MAX_PATH);
    // Near the limit: whatever succeeds fits the buffer; one byte more than the
    // buffer allows must fail.
    char fit[MINIMAFS_MAX_PATH + 8];
    n = 0;
    while (n < 1020) { fit[n++] = '/'; for (int k = 0; k < 99 && n < 1020; k++) fit[n++] = 'e'; }
    fit[n] = '\0';
    if (fs_resolve_path_from("0:/", fit, out)) CHECK(strlen(out) <= MINIMAFS_MAX_PATH - 1);
    fit[n++] = 'z'; fit[n++] = 'z'; fit[n++] = 'z'; fit[n++] = 'z'; fit[n] = '\0';
    CHECK(!fs_resolve_path_from("0:/", fit, out));
}

static const char* from_user(const uint8_t* region, uintptr_t user_base, const char* p) {
    return (const char*)(region + ((uintptr_t)p - user_base));
}

static void test_argblock(void) {
    const char* argv[] = {"prog", "-x", "hello world"};
    static const char env[] = "A=1\0B=2\0";
    CHECK_EQ_I(argblock_env_count(env, sizeof env - 1), 2);
    CHECK_EQ_I(argblock_env_count(NULL, 5), 0);
    CHECK_EQ_I(argblock_env_count(env, 0), 0);
    static const char envgaps[] = "\0A=1\0\0\0B=2\0C=3";           // empty entries, no trailing NUL
    CHECK_EQ_I(argblock_env_count(envgaps, sizeof envgaps - 1), 3);

    size_t ptrs = (3 + 1 + 2 + 1) * sizeof(char*);
    size_t strs = 5 + 3 + 12 + 4 + 4;
    CHECK_EQ_I(argblock_size(3, argv, env, sizeof env - 1), ptrs + strs);

    uint64_t storage[128];
    uint8_t* region = (uint8_t*)storage;
    const uintptr_t ub = 0x7ffff000u;
    char** av = NULL; char** ep = NULL;
    memset(storage, 0xAA, sizeof storage);
    CHECK(argblock_build(region, sizeof storage, ub, 3, argv, env, sizeof env - 1, &av, &ep));
    CHECK((uintptr_t)av == ub);
    CHECK((uintptr_t)ep == ub + (3 + 1) * sizeof(char*));
    char** av_h = (char**)region;
    char** ep_h = (char**)(region + ((uintptr_t)ep - ub));
    CHECK_STR(from_user(region, ub, av_h[0]), "prog");
    CHECK_STR(from_user(region, ub, av_h[1]), "-x");
    CHECK_STR(from_user(region, ub, av_h[2]), "hello world");
    CHECK(av_h[3] == NULL);
    CHECK_STR(from_user(region, ub, ep_h[0]), "A=1");
    CHECK_STR(from_user(region, ub, ep_h[1]), "B=2");
    CHECK(ep_h[2] == NULL);
    for (int i = 0; i < 3; i++) {
        uintptr_t off = (uintptr_t)av_h[i] - ub;
        CHECK(off >= ptrs && off < ptrs + strs);
    }
    for (int i = 0; i < 2; i++) {
        uintptr_t off = (uintptr_t)ep_h[i] - ub;
        CHECK(off >= ptrs && off < ptrs + strs);
    }

    // Exact fit works, one byte short fails.
    size_t need = argblock_size(3, argv, env, sizeof env - 1);
    CHECK(argblock_build(region, need, ub, 3, argv, env, sizeof env - 1, &av, &ep));
    CHECK(!argblock_build(region, need - 1, ub, 3, argv, env, sizeof env - 1, &av, &ep));
    CHECK(!argblock_build(region, 0, ub, 3, argv, env, sizeof env - 1, &av, &ep));
    CHECK(!argblock_build(NULL, need, ub, 3, argv, env, sizeof env - 1, &av, &ep));

    // Empty: argc 0, no env.
    CHECK(argblock_build(region, sizeof storage, ub, 0, NULL, NULL, 0, &av, &ep));
    CHECK_EQ_I(argblock_size(0, NULL, NULL, 0), 2 * sizeof(char*));
    char** a0 = (char**)region;
    CHECK(a0[0] == NULL && a0[1] == NULL);
    CHECK((uintptr_t)ep == ub + sizeof(char*));

    // NULL argv entries become "", gaps in the env are skipped.
    const char* argv2[] = {"a", NULL, "c"};
    CHECK(argblock_build(region, sizeof storage, ub, 3, argv2, envgaps, sizeof envgaps - 1, &av, &ep));
    av_h = (char**)region;
    ep_h = (char**)(region + ((uintptr_t)ep - ub));
    CHECK_STR(from_user(region, ub, av_h[0]), "a");
    CHECK_STR(from_user(region, ub, av_h[1]), "");
    CHECK_STR(from_user(region, ub, av_h[2]), "c");
    CHECK_STR(from_user(region, ub, ep_h[0]), "A=1");
    CHECK_STR(from_user(region, ub, ep_h[1]), "B=2");
    CHECK_STR(from_user(region, ub, ep_h[2]), "C=3");
    CHECK(ep_h[3] == NULL);
    CHECK_EQ_I(argblock_size(3, argv2, envgaps, sizeof envgaps - 1),
               (3 + 1 + 3 + 1) * sizeof(char*) + 2 + 1 + 2 + 4 * 3);
    // NULL argv array with argc > 0: all entries are empty.
    CHECK(argblock_build(region, sizeof storage, ub, 2, NULL, NULL, 0, &av, &ep));
    av_h = (char**)region;
    CHECK_STR(from_user(region, ub, av_h[0]), "");
    CHECK_STR(from_user(region, ub, av_h[1]), "");

    // Limits.
    CHECK_EQ_I(argblock_size(ARGBLOCK_MAX_ARGS + 1, NULL, NULL, 0), 0);
    size_t big = argblock_size(ARGBLOCK_MAX_ARGS, NULL, NULL, 0);
    CHECK(big > 0);
    char* hbuf = malloc(big + ARGBLOCK_MAX_ARGS);
    CHECK(hbuf != NULL);
    if (hbuf) {
        CHECK(argblock_build(hbuf, big, ub, ARGBLOCK_MAX_ARGS, NULL, NULL, 0, &av, &ep));
        CHECK(!argblock_build(hbuf, big, ub, ARGBLOCK_MAX_ARGS + 1, NULL, NULL, 0, &av, &ep));
        free(hbuf);
    }
    // Environment with too many entries: "e\0" repeated.
    size_t elen = (size_t)(ARGBLOCK_MAX_ENV + 1) * 2;
    char* e1 = malloc(elen);
    if (e1) {
        for (size_t i = 0; i < elen; i += 2) { e1[i] = 'e'; e1[i + 1] = 0; }
        CHECK_EQ_I(argblock_env_count(e1, (uint32_t)elen), ARGBLOCK_MAX_ENV + 1);
        CHECK_EQ_I(argblock_size(0, NULL, e1, (uint32_t)elen), 0);
        CHECK(argblock_size(0, NULL, e1, (uint32_t)(elen - 2)) > 0);          // exactly MAX_ENV entries
        CHECK(!argblock_build(region, sizeof storage, ub, 0, NULL, e1, (uint32_t)elen, &av, &ep));
        free(e1);
    }
}

int main(void) {
    test_civil();
    test_date_parse();
    test_ticks();
    test_paths();
    test_argblock();
    printf("os0_tests: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { printf("FAILED\n"); return 1; }
    printf("PASSED\n");
    return 0;
}
