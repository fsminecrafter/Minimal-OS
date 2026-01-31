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

// Initialize time subsystem
void time_init(void);

// Update system time (call from PIT interrupt)
void time_tick(uint32_t pit_frequency);

// Get system uptime
uint64_t time_get_uptime_ms(void);
uint64_t time_get_uptime_sec(void);

// Get current date/time
datetime_t time_get_datetime(void);

// Format functions
void time_format_time(char* buffer, size_t bufsize);      // HH:MM:SS
void time_format_date(char* buffer, size_t bufsize);      // YYYY-MM-DD
void time_format_datetime(char* buffer, size_t bufsize);  // YYYY-MM-DD HH:MM:SS
void time_format_uptime(char* buffer, size_t bufsize);    // Xd Xh Xm Xs

// Print functions
void time_print_datetime(bool show_uptime);

#endif // TIME_H