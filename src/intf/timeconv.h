#ifndef TIMECONV_H
#define TIMECONV_H

/*
 * Time arithmetic with no hardware behind it: calendar dates to Unix time,
 * MinimaFS date strings, and turning TSC ticks into nanoseconds. Kept apart
 * from time.c (RTC, PIT) and clock.c (CPUID, RDTSC) so tests/host can check
 * it on the build machine.
 */

#include <stdint.h>
#include <stdbool.h>

// Seconds since 1970-01-01 00:00:00 for a proleptic Gregorian date and time
// (month 1-12, day 1-31). No range checks beyond what the formula needs.
int64_t timeconv_unix_from_civil(int64_t year, int month, int day,
                                 int hour, int minute, int second);

// A MinimaFS date ("12Nov2026", as minimafs_get_datetime() writes it) as
// seconds since 1970 at midnight of that day. False for anything else.
bool timeconv_parse_minimafs_date(const char* text, int64_t* out_seconds);

// Fixed-point factor for timeconv_ticks_to_ns(): nanoseconds per tick in
// 32.32 form. 0 when hz is 0.
uint64_t timeconv_ns_per_tick_q32(uint64_t hz);

// ticks * ns_per_tick_q32 >> 32, exact for any 64-bit tick count.
static inline uint64_t timeconv_ticks_to_ns(uint64_t ticks, uint64_t ns_per_tick_q32) {
    return (uint64_t)(((unsigned __int128)ticks * ns_per_tick_q32) >> 32);
}

#endif // TIMECONV_H
