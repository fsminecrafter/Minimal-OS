#include "timeconv.h"

// Howard Hinnant's days_from_civil: days since 1970-01-01.
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                   // [0, 399]
    const int64_t mp = (m + 9) % 12;                                     // March = 0
    const int64_t doy = (153 * mp + 2) / 5 + d - 1;                      // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + doe - 719468;
}

int64_t timeconv_unix_from_civil(int64_t year, int month, int day,
                                 int hour, int minute, int second) {
    return days_from_civil(year, month, day) * 86400 +
           (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool timeconv_parse_minimafs_date(const char* text, int64_t* out_seconds) {
    static const char months[12][4] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec",
    };
    if (!text || !out_seconds) return false;

    // DD
    if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') return false;
    int day = (text[0] - '0') * 10 + (text[1] - '0');

    // Mon
    int month = 0;
    for (int i = 0; i < 12 && !month; i++) {
        if (ascii_lower(text[2]) == months[i][0] && ascii_lower(text[3]) == months[i][1] &&
            ascii_lower(text[4]) == months[i][2])
            month = i + 1;
    }
    if (!month) return false;

    // YYYY
    int year = 0;
    for (int i = 5; i < 9; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        year = year * 10 + (text[i] - '0');
    }
    if (day < 1 || day > 31) return false;

    *out_seconds = timeconv_unix_from_civil(year, month, day, 0, 0, 0);
    return true;
}

uint64_t timeconv_ns_per_tick_q32(uint64_t hz) {
    if (!hz) return 0;
    return (uint64_t)(((unsigned __int128)1000000000u << 32) / hz);
}
