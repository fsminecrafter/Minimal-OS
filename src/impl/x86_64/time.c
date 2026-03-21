#include <stdint.h>
#include <stdbool.h>

#include "time.h"
#include "x86_64/rtc.h"
#include "x86_64/pit.h"
#include "print.h"
#include "serial.h"
#include "string.h"

// System uptime tracking
static uint64_t system_uptime_ms = 0;
static uint64_t last_pit_ticks = 0;

// Current date/time cache
static datetime_t current_datetime = {0};
static uint64_t last_rtc_update_ms = 0;

// Static buffers for string returns (4 rotating buffers)
static char time_str_bufs[4][64];
static int time_str_idx = 0;

// ===========================================
// HELPER FUNCTIONS
// ===========================================

// RTC hardware returns values in BCD (Binary Coded Decimal).
// e.g. 0x26 means "26" not decimal 38.
// Decode: high nibble * 10 + low nibble.
static inline uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Year comes back as a 2-digit BCD value (e.g. 0x26 = year 26 of century).
// Century is typically 20, so full year = 2000 + bcd_to_bin(raw_year).
// If rtc_year() already returns a 16-bit full year this is still safe
// because bcd_to_bin(0x26) = 26 and 2000+26 = 2026.
static inline uint16_t bcd_to_year(uint8_t raw) {
    uint8_t two_digit = bcd_to_bin(raw);
    // Assume 21st century. If the decoded value is >= 70 it's likely 19xx
    // (handles RTC chips that hold years 70-99 as 1970-1999).
    return (two_digit >= 70) ? (uint16_t)(1900 + two_digit)
                             : (uint16_t)(2000 + two_digit);
}

static char* get_str_buffer(void) {
    char* buf = time_str_bufs[time_str_idx++ % 4];
    return buf;
}

// Encode binary back to BCD for writing to the RTC chip
static inline uint8_t bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}

static void write_rtc_if_needed(void) {
    datetime_t rtc_raw;
    rtc_get_datetime(&rtc_raw);

    // rtc_raw fields are BCD — decode before comparing to our binary values
    if (bcd_to_bin(rtc_raw.second) != current_datetime.second)
        rtc_write_seconds(bin_to_bcd(current_datetime.second));

    if (bcd_to_bin(rtc_raw.minute) != current_datetime.minute)
        rtc_write_minutes(bin_to_bcd(current_datetime.minute));

    if (bcd_to_bin(rtc_raw.hour) != current_datetime.hour)
        rtc_write_hours(bin_to_bcd(current_datetime.hour));

    if (bcd_to_bin(rtc_raw.day) != current_datetime.day)
        rtc_write_day(bin_to_bcd(current_datetime.day));

    if (bcd_to_bin(rtc_raw.month) != current_datetime.month)
        rtc_write_month(bin_to_bcd(current_datetime.month));

    // Year: write back the 2-digit BCD form of the current year
    uint8_t year_2digit = (uint8_t)(current_datetime.year % 100);
    if (bcd_to_year(rtc_raw.year) != current_datetime.year)
        rtc_write_year(bin_to_bcd(year_2digit));
}
// ===========================================
// INITIALIZATION & UPDATES
// ===========================================

void time_init(void) {
    serial_write_str("Initializing time subsystem...\n");
    
    // Read initial RTC values — all fields are BCD-encoded on real hardware
    current_datetime.year   = bcd_to_year(rtc_year());
    current_datetime.month  = bcd_to_bin(rtc_month());
    current_datetime.day    = bcd_to_bin(rtc_day());
    current_datetime.hour   = bcd_to_bin(rtc_hours());
    current_datetime.minute = bcd_to_bin(rtc_minutes());
    current_datetime.second = bcd_to_bin(rtc_seconds());
    
    system_uptime_ms = 0;
    last_pit_ticks = 0;
    last_rtc_update_ms = 0;
    
    serial_write_str("Time initialized: ");
    serial_write_str(datetime_str());
    serial_write_str("\n");
}

void time_tick(uint32_t pit_frequency) {
    uint64_t current_ticks = pit_get_ticks();
    uint64_t elapsed_ticks = current_ticks - last_pit_ticks;
    last_pit_ticks = current_ticks;
    
    uint64_t ms_elapsed = (elapsed_ticks * 1000) / pit_frequency;
    system_uptime_ms += ms_elapsed;
    
    // Update RTC cache every second
    if (system_uptime_ms - last_rtc_update_ms >= 1000) {
        last_rtc_update_ms = system_uptime_ms;
        
        // Re-read RTC — all fields are BCD-encoded
        current_datetime.second = bcd_to_bin(rtc_seconds());
        current_datetime.minute = bcd_to_bin(rtc_minutes());
        current_datetime.hour   = bcd_to_bin(rtc_hours());
        current_datetime.day    = bcd_to_bin(rtc_day());
        current_datetime.month  = bcd_to_bin(rtc_month());
        current_datetime.year   = bcd_to_year(rtc_year());
    }
}

// ===========================================
// GETTERS - Values
// ===========================================

uint64_t time_get_uptime_ms(void) {
    return system_uptime_ms;
}

uint64_t time_get_uptime_sec(void) {
    return system_uptime_ms / 1000;
}

datetime_t time_get_datetime(void) {
    return current_datetime;
}

uint16_t time_get_year(void) {
    return current_datetime.year;
}

uint8_t time_get_month(void) {
    return current_datetime.month;
}

uint8_t time_get_day(void) {
    return current_datetime.day;
}

uint8_t time_get_hour(void) {
    return current_datetime.hour;
}

uint8_t time_get_minute(void) {
    return current_datetime.minute;
}

uint8_t time_get_second(void) {
    return current_datetime.second;
}

// Zeller's congruence algorithm
uint8_t time_get_day_of_week(void) {
    int y = current_datetime.year;
    int m = current_datetime.month;
    int d = current_datetime.day;
    
    if (m < 3) {
        m += 12;
        y--;
    }
    
    int h = (d + (13 * (m + 1)) / 5 + y + y / 4 - y / 100 + y / 400) % 7;
    return (h + 6) % 7;  // Convert to Sunday=0
}

// ===========================================
// GETTERS - Formatted Strings
// ===========================================

const char* time_str(void) {
    char* buf = get_str_buffer();
    time_format_time(buf, 64);
    return buf;
}

const char* time_str_12h(void) {
    char* buf = get_str_buffer();
    uint8_t hour = current_datetime.hour;
    const char* ampm = (hour >= 12) ? "PM" : "AM";
    
    if (hour == 0) hour = 12;
    else if (hour > 12) hour -= 12;
    
    buf[0] = '0' + (hour / 10);
    buf[1] = '0' + (hour % 10);
    buf[2] = ':';
    buf[3] = '0' + (current_datetime.minute / 10);
    buf[4] = '0' + (current_datetime.minute % 10);
    buf[5] = ':';
    buf[6] = '0' + (current_datetime.second / 10);
    buf[7] = '0' + (current_datetime.second % 10);
    buf[8] = ' ';
    buf[9] = ampm[0];
    buf[10] = ampm[1];
    buf[11] = '\0';
    
    return buf;
}

const char* date_str(void) {
    char* buf = get_str_buffer();
    time_format_date(buf, 64);
    return buf;
}

const char* date_str_us(void) {
    char* buf = get_str_buffer();
    // MM/DD/YYYY
    buf[0] = '0' + (current_datetime.month / 10);
    buf[1] = '0' + (current_datetime.month % 10);
    buf[2] = '/';
    buf[3] = '0' + (current_datetime.day / 10);
    buf[4] = '0' + (current_datetime.day % 10);
    buf[5] = '/';
    uint16_t year = current_datetime.year;
    buf[6] = '0' + (year / 1000);
    buf[7] = '0' + ((year / 100) % 10);
    buf[8] = '0' + ((year / 10) % 10);
    buf[9] = '0' + (year % 10);
    buf[10] = '\0';
    return buf;
}

const char* date_str_eu(void) {
    char* buf = get_str_buffer();
    // DD/MM/YYYY
    buf[0] = '0' + (current_datetime.day / 10);
    buf[1] = '0' + (current_datetime.day % 10);
    buf[2] = '/';
    buf[3] = '0' + (current_datetime.month / 10);
    buf[4] = '0' + (current_datetime.month % 10);
    buf[5] = '/';
    uint16_t year = current_datetime.year;
    buf[6] = '0' + (year / 1000);
    buf[7] = '0' + ((year / 100) % 10);
    buf[8] = '0' + ((year / 10) % 10);
    buf[9] = '0' + (year % 10);
    buf[10] = '\0';
    return buf;
}

const char* datetime_str(void) {
    char* buf = get_str_buffer();
    time_format_datetime(buf, 64);
    return buf;
}

const char* datetime_str_readable(void) {
    char* buf = get_str_buffer();
    // "Mon, Jan 15 2025 14:30:45"
    
    const char* day_name = time_get_weekday_name_short(time_get_day_of_week());
    const char* month_name = time_get_month_name_short(current_datetime.month);
    
    int pos = 0;
    
    // Day name
    while (*day_name && pos < 60) buf[pos++] = *day_name++;
    buf[pos++] = ',';
    buf[pos++] = ' ';
    
    // Month name
    while (*month_name && pos < 60) buf[pos++] = *month_name++;
    buf[pos++] = ' ';
    
    // Day
    buf[pos++] = '0' + (current_datetime.day / 10);
    buf[pos++] = '0' + (current_datetime.day % 10);
    buf[pos++] = ' ';
    
    // Year
    uint16_t year = current_datetime.year;
    buf[pos++] = '0' + (year / 1000);
    buf[pos++] = '0' + ((year / 100) % 10);
    buf[pos++] = '0' + ((year / 10) % 10);
    buf[pos++] = '0' + (year % 10);
    buf[pos++] = ' ';
    
    // Time
    buf[pos++] = '0' + (current_datetime.hour / 10);
    buf[pos++] = '0' + (current_datetime.hour % 10);
    buf[pos++] = ':';
    buf[pos++] = '0' + (current_datetime.minute / 10);
    buf[pos++] = '0' + (current_datetime.minute % 10);
    buf[pos++] = ':';
    buf[pos++] = '0' + (current_datetime.second / 10);
    buf[pos++] = '0' + (current_datetime.second % 10);
    
    buf[pos] = '\0';
    return buf;
}

const char* uptime_str(void) {
    char* buf = get_str_buffer();
    time_format_uptime(buf, 64);
    return buf;
}

const char* uptime_str_short(void) {
    char* buf = get_str_buffer();
    // "D:HH:MM:SS"
    
    uint64_t total_sec = system_uptime_ms / 1000;
    uint64_t days = total_sec / 86400;
    uint64_t hours = (total_sec % 86400) / 3600;
    uint64_t minutes = (total_sec % 3600) / 60;
    uint64_t seconds = total_sec % 60;
    
    int pos = 0;
    
    // Days
    char temp[32];
    uint_to_str(days, temp);
    for (int i = 0; temp[i]; i++) buf[pos++] = temp[i];
    buf[pos++] = ':';
    
    // Hours (2 digits)
    buf[pos++] = '0' + (hours / 10);
    buf[pos++] = '0' + (hours % 10);
    buf[pos++] = ':';
    
    // Minutes (2 digits)
    buf[pos++] = '0' + (minutes / 10);
    buf[pos++] = '0' + (minutes % 10);
    buf[pos++] = ':';
    
    // Seconds (2 digits)
    buf[pos++] = '0' + (seconds / 10);
    buf[pos++] = '0' + (seconds % 10);
    
    buf[pos] = '\0';
    return buf;
}

const char* uptime_str_human(void) {
    char* buf = get_str_buffer();
    // "5 days, 3 hours"
    
    uint64_t total_sec = system_uptime_ms / 1000;
    uint64_t days = total_sec / 86400;
    uint64_t hours = (total_sec % 86400) / 3600;
    
    int pos = 0;
    char temp[32];
    
    if (days > 0) {
        uint_to_str(days, temp);
        for (int i = 0; temp[i] && pos < 60; i++) buf[pos++] = temp[i];
        
        if (days == 1) {
            buf[pos++] = ' ';
            buf[pos++] = 'd';
            buf[pos++] = 'a';
            buf[pos++] = 'y';
        } else {
            buf[pos++] = ' ';
            buf[pos++] = 'd';
            buf[pos++] = 'a';
            buf[pos++] = 'y';
            buf[pos++] = 's';
        }
        
        if (hours > 0) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
    }
    
    if (hours > 0 || days > 0) {
        uint_to_str(hours, temp);
        for (int i = 0; temp[i] && pos < 60; i++) buf[pos++] = temp[i];
        
        if (hours == 1) {
            buf[pos++] = ' ';
            buf[pos++] = 'h';
            buf[pos++] = 'o';
            buf[pos++] = 'u';
            buf[pos++] = 'r';
        } else {
            buf[pos++] = ' ';
            buf[pos++] = 'h';
            buf[pos++] = 'o';
            buf[pos++] = 'u';
            buf[pos++] = 'r';
            buf[pos++] = 's';
        }
    }
    
    if (days == 0 && hours == 0) {
        uint64_t minutes = (total_sec % 3600) / 60;
        uint_to_str(minutes, temp);
        for (int i = 0; temp[i] && pos < 60; i++) buf[pos++] = temp[i];
        
        if (minutes == 1) {
            buf[pos++] = ' ';
            buf[pos++] = 'm';
            buf[pos++] = 'i';
            buf[pos++] = 'n';
        } else {
            buf[pos++] = ' ';
            buf[pos++] = 'm';
            buf[pos++] = 'i';
            buf[pos++] = 'n';
            buf[pos++] = 's';
        }
    }
    
    buf[pos] = '\0';
    return buf;
}

// Month names
const char* time_get_month_name(uint8_t month) {
    static const char* names[] = {
        "Invalid", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    return (month >= 1 && month <= 12) ? names[month] : names[0];
}

const char* time_get_month_name_short(uint8_t month) {
    static const char* names[] = {
        "Inv", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    return (month >= 1 && month <= 12) ? names[month] : names[0];
}

// Weekday names
const char* time_get_weekday_name(uint8_t day) {
    static const char* names[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    return (day <= 6) ? names[day] : "Invalid";
}

const char* time_get_weekday_name_short(uint8_t day) {
    static const char* names[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    return (day <= 6) ? names[day] : "Inv";
}

// ===========================================
// SETTERS
// ===========================================

void time_set_datetime(const datetime_t* dt) {
    if (time_validate_datetime(dt)) {
        current_datetime = *dt;
        write_rtc_if_needed();
    }
}

void time_set_year(uint16_t year) {
    current_datetime.year = year;
    write_rtc_if_needed();
}

void time_set_month(uint8_t month) {
    if (month >= 1 && month <= 12) {
        current_datetime.month = month;
        write_rtc_if_needed();
    }
}

void time_set_day(uint8_t day) {
    if (day >= 1 && day <= 31) {
        current_datetime.day = day;
        write_rtc_if_needed();
    }
}

void time_set_hour(uint8_t hour) {
    if (hour < 24) {
        current_datetime.hour = hour;
        write_rtc_if_needed();
    }
}

void time_set_minute(uint8_t minute) {
    if (minute < 60) {
        current_datetime.minute = minute;
        write_rtc_if_needed();
    }
}

void time_set_second(uint8_t second) {
    if (second < 60) {
        current_datetime.second = second;
        write_rtc_if_needed();
    }
}

void time_set_date(uint16_t year, uint8_t month, uint8_t day) {
    current_datetime.year = year;
    current_datetime.month = month;
    current_datetime.day = day;
    write_rtc_if_needed();
}

void time_set_time(uint8_t hour, uint8_t minute, uint8_t second) {
    current_datetime.hour = hour;
    current_datetime.minute = minute;
    current_datetime.second = second;
    write_rtc_if_needed();
}

// Parse "YYYY-MM-DD HH:MM:SS"
bool time_set_from_str(const char* datetime_str) {
    if (!datetime_str || strlen(datetime_str) < 19) return false;
    
    // Parse year
    current_datetime.year = (datetime_str[0] - '0') * 1000 +
                            (datetime_str[1] - '0') * 100 +
                            (datetime_str[2] - '0') * 10 +
                            (datetime_str[3] - '0');
    
    // Parse month
    current_datetime.month = (datetime_str[5] - '0') * 10 +
                             (datetime_str[6] - '0');
    
    // Parse day
    current_datetime.day = (datetime_str[8] - '0') * 10 +
                           (datetime_str[9] - '0');
    
    // Parse hour
    current_datetime.hour = (datetime_str[11] - '0') * 10 +
                            (datetime_str[12] - '0');
    
    // Parse minute
    current_datetime.minute = (datetime_str[14] - '0') * 10 +
                              (datetime_str[15] - '0');
    
    // Parse second
    current_datetime.second = (datetime_str[17] - '0') * 10 +
                              (datetime_str[18] - '0');
    
    if (time_validate_datetime(&current_datetime)) {
        write_rtc_if_needed();
        return true;
    }
    
    return false;
}

// ===========================================
// LEGACY FORMAT FUNCTIONS
// ===========================================

void time_format_time(char* buffer, size_t bufsize) {
    if (!buffer || bufsize < 9) return;
    
    buffer[0] = '0' + (current_datetime.hour / 10);
    buffer[1] = '0' + (current_datetime.hour % 10);
    buffer[2] = ':';
    buffer[3] = '0' + (current_datetime.minute / 10);
    buffer[4] = '0' + (current_datetime.minute % 10);
    buffer[5] = ':';
    buffer[6] = '0' + (current_datetime.second / 10);
    buffer[7] = '0' + (current_datetime.second % 10);
    buffer[8] = '\0';
}

void time_format_date(char* buffer, size_t bufsize) {
    if (!buffer || bufsize < 11) return;
    
    uint16_t year = current_datetime.year;
    buffer[0] = '0' + (year / 1000);
    buffer[1] = '0' + ((year / 100) % 10);
    buffer[2] = '0' + ((year / 10) % 10);
    buffer[3] = '0' + (year % 10);
    buffer[4] = '-';
    buffer[5] = '0' + (current_datetime.month / 10);
    buffer[6] = '0' + (current_datetime.month % 10);
    buffer[7] = '-';
    buffer[8] = '0' + (current_datetime.day / 10);
    buffer[9] = '0' + (current_datetime.day % 10);
    buffer[10] = '\0';
}

void time_format_datetime(char* buffer, size_t bufsize) {
    if (!buffer || bufsize < 20) return;
    
    char date[11];
    char time[9];
    
    time_format_date(date, sizeof(date));
    time_format_time(time, sizeof(time));
    
    int i = 0;
    for (int j = 0; date[j] && i < bufsize - 1; j++, i++) {
        buffer[i] = date[j];
    }
    if (i < bufsize - 1) buffer[i++] = ' ';
    for (int j = 0; time[j] && i < bufsize - 1; j++, i++) {
        buffer[i] = time[j];
    }
    buffer[i] = '\0';
}

void time_format_uptime(char* buffer, size_t bufsize) {
    if (!buffer || bufsize < 20) return;
    
    uint64_t total_sec = system_uptime_ms / 1000;
    uint64_t days = total_sec / 86400;
    uint64_t hours = (total_sec % 86400) / 3600;
    uint64_t minutes = (total_sec % 3600) / 60;
    uint64_t seconds = total_sec % 60;
    
    char temp[64];
    int pos = 0;
    
    if (days > 0) {
        uint_to_str(days, temp);
        for (int i = 0; temp[i] && pos < bufsize - 2; i++, pos++) {
            buffer[pos] = temp[i];
        }
        buffer[pos++] = 'd';
        buffer[pos++] = ' ';
    }
    
    if (hours > 0 || days > 0) {
        uint_to_str(hours, temp);
        for (int i = 0; temp[i] && pos < bufsize - 2; i++, pos++) {
            buffer[pos] = temp[i];
        }
        buffer[pos++] = 'h';
        buffer[pos++] = ' ';
    }
    
    if (minutes > 0 || hours > 0 || days > 0) {
        uint_to_str(minutes, temp);
        for (int i = 0; temp[i] && pos < bufsize - 2; i++, pos++) {
            buffer[pos] = temp[i];
        }
        buffer[pos++] = 'm';
        buffer[pos++] = ' ';
    }
    
    uint_to_str(seconds, temp);
    for (int i = 0; temp[i] && pos < bufsize - 1; i++, pos++) {
        buffer[pos] = temp[i];
    }
    buffer[pos++] = 's';
    buffer[pos] = '\0';
}

// ===========================================
// UTILITIES
// ===========================================

bool time_is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t time_days_in_month(uint8_t month, uint16_t year) {
    static const uint8_t days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && time_is_leap_year(year)) return 29;
    return days[month];
}

bool time_validate_datetime(const datetime_t* dt) {
    if (dt->month < 1 || dt->month > 12) return false;
    if (dt->day < 1 || dt->day > time_days_in_month(dt->month, dt->year)) return false;
    if (dt->hour >= 24) return false;
    if (dt->minute >= 60) return false;
    if (dt->second >= 60) return false;
    return true;
}

void time_ms_to_duration(uint64_t ms, duration_t* dur) {
    dur->milliseconds = ms % 1000;
    uint64_t total_sec = ms / 1000;
    dur->seconds = total_sec % 60;
    uint64_t total_min = total_sec / 60;
    dur->minutes = total_min % 60;
    uint64_t total_hours = total_min / 60;
    dur->hours = total_hours % 24;
    dur->days = total_hours / 24;
}

// ===========================================
// PRINT FUNCTIONS
// ===========================================

void time_print_datetime(bool show_uptime) {
    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("[");
    print_str(datetime_str());
    print_str("]");
    
    if (show_uptime) {
        print_str(" [Uptime: ");
        print_str(uptime_str());
        print_str("]");
    }
    
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
}

void time_print_date(void) {
    print_str(date_str());
}

void time_print_time(void) {
    print_str(time_str());
}

void time_print_uptime(void) {
    print_str(uptime_str_human());
}