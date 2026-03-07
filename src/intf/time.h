#ifndef TIME_H
#define TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Date/time structure
typedef struct {
    uint16_t year;
    uint8_t month;    // 1-12
    uint8_t day;      // 1-31
    uint8_t hour;     // 0-23
    uint8_t minute;   // 0-59
    uint8_t second;   // 0-59
} datetime_t;

// Duration structure for easy time arithmetic
typedef struct {
    uint64_t days;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
    uint16_t milliseconds;
} duration_t;

// ===========================================
// INITIALIZATION & UPDATES
// ===========================================

// Initialize time subsystem
void time_init(void);

// Update system time (call from PIT interrupt)
void time_tick(uint32_t pit_frequency);

// ===========================================
// GETTERS - Values
// ===========================================

// Get system uptime
uint64_t time_get_uptime_ms(void);
uint64_t time_get_uptime_sec(void);

// Get current date/time
datetime_t time_get_datetime(void);

// Get individual components
uint16_t time_get_year(void);
uint8_t time_get_month(void);
uint8_t time_get_day(void);
uint8_t time_get_hour(void);
uint8_t time_get_minute(void);
uint8_t time_get_second(void);

// Get day of week (0 = Sunday, 6 = Saturday)
uint8_t time_get_day_of_week(void);

// ===========================================
// GETTERS - Formatted Strings (Easy!)
// ===========================================

// These return static strings - use immediately or copy!

const char* time_str(void);              // "HH:MM:SS"
const char* time_str_12h(void);          // "HH:MM:SS AM/PM"
const char* date_str(void);              // "YYYY-MM-DD"
const char* date_str_us(void);           // "MM/DD/YYYY"
const char* date_str_eu(void);           // "DD/MM/YYYY"
const char* datetime_str(void);          // "YYYY-MM-DD HH:MM:SS"
const char* datetime_str_readable(void); // "Mon, Jan 15 2025 14:30:45"
const char* uptime_str(void);            // "5d 3h 42m 18s"
const char* uptime_str_short(void);      // "5:03:42:18"
const char* uptime_str_human(void);      // "5 days, 3 hours"

// Month and weekday names
const char* time_get_month_name(uint8_t month);       // "January"
const char* time_get_month_name_short(uint8_t month); // "Jan"
const char* time_get_weekday_name(uint8_t day);       // "Monday"
const char* time_get_weekday_name_short(uint8_t day); // "Mon"

// ===========================================
// SETTERS - Set Date/Time
// ===========================================

// Set complete datetime
void time_set_datetime(const datetime_t* dt);

// Set individual components
void time_set_year(uint16_t year);
void time_set_month(uint8_t month);
void time_set_day(uint8_t day);
void time_set_hour(uint8_t hour);
void time_set_minute(uint8_t minute);
void time_set_second(uint8_t second);

// Set date only
void time_set_date(uint16_t year, uint8_t month, uint8_t day);

// Set time only
void time_set_time(uint8_t hour, uint8_t minute, uint8_t second);

// Parse and set from string
bool time_set_from_str(const char* datetime_str);  // "YYYY-MM-DD HH:MM:SS"
bool time_set_date_from_str(const char* date_str); // "YYYY-MM-DD"
bool time_set_time_from_str(const char* time_str); // "HH:MM:SS"

// ===========================================
// LEGACY FORMAT FUNCTIONS (Buffer versions)
// ===========================================

void time_format_time(char* buffer, size_t bufsize);      // HH:MM:SS
void time_format_date(char* buffer, size_t bufsize);      // YYYY-MM-DD
void time_format_datetime(char* buffer, size_t bufsize);  // YYYY-MM-DD HH:MM:SS
void time_format_uptime(char* buffer, size_t bufsize);    // Xd Xh Xm Xs

// ===========================================
// TIME UTILITIES
// ===========================================

// Check if year is leap year
bool time_is_leap_year(uint16_t year);

// Get days in month
uint8_t time_days_in_month(uint8_t month, uint16_t year);

// Validate datetime
bool time_validate_datetime(const datetime_t* dt);

// Convert uptime to duration
void time_ms_to_duration(uint64_t ms, duration_t* dur);

// Time arithmetic
void time_add_seconds(datetime_t* dt, uint64_t seconds);
void time_add_minutes(datetime_t* dt, uint32_t minutes);
void time_add_hours(datetime_t* dt, uint32_t hours);
void time_add_days(datetime_t* dt, uint32_t days);

// Compare datetimes
int time_compare(const datetime_t* dt1, const datetime_t* dt2);

// ===========================================
// PRINT FUNCTIONS
// ===========================================

void time_print_datetime(bool show_uptime);
void time_print_date(void);
void time_print_time(void);
void time_print_uptime(void);

#endif // TIME_H