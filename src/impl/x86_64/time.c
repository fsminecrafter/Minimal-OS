#include "time.h"
#include "x86_64/rtc.h"
#include "x86_64/pit.h"
#include "print.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

// System uptime tracking
static uint64_t system_uptime_ms = 0;
static uint64_t last_pit_ticks = 0;

// Current date/time cache
static datetime_t current_datetime = {0};
static uint64_t last_rtc_update_ms = 0;

// Initialize time subsystem
void time_init(void) {
    serial_write_str("Initializing time subsystem...\n");
    
    // Read initial RTC values
    current_datetime.year = rtc_year();
    current_datetime.month = rtc_month();
    current_datetime.day = rtc_day();
    current_datetime.hour = rtc_hours();
    current_datetime.minute = rtc_minutes();
    current_datetime.second = rtc_seconds();
    
    system_uptime_ms = 0;
    last_pit_ticks = 0;
    last_rtc_update_ms = 0;
    
    serial_write_str("Time initialized: ");
    serial_write_dec(current_datetime.year);
    serial_write_str("-");
    serial_write_dec(current_datetime.month);
    serial_write_str("-");
    serial_write_dec(current_datetime.day);
    serial_write_str(" ");
    serial_write_dec(current_datetime.hour);
    serial_write_str(":");
    serial_write_dec(current_datetime.minute);
    serial_write_str(":");
    serial_write_dec(current_datetime.second);
    serial_write_str("\n");
}

// Update system time (call this from PIT interrupt handler)
void time_tick(uint32_t pit_frequency) {
    uint64_t current_ticks = pit_get_ticks();
    uint64_t elapsed_ticks = current_ticks - last_pit_ticks;
    last_pit_ticks = current_ticks;
    
    // Calculate milliseconds elapsed
    // pit_frequency is in Hz, so ms_per_tick = 1000 / frequency
    uint64_t ms_elapsed = (elapsed_ticks * 1000) / pit_frequency;
    system_uptime_ms += ms_elapsed;
    
    // Update RTC cache every second to reduce RTC reads
    if (system_uptime_ms - last_rtc_update_ms >= 1000) {
        last_rtc_update_ms = system_uptime_ms;
        
        current_datetime.second = rtc_seconds();
        current_datetime.minute = rtc_minutes();
        current_datetime.hour = rtc_hours();
        current_datetime.day = rtc_day();
        current_datetime.month = rtc_month();
        current_datetime.year = rtc_year();
    }
}

// Get system uptime in milliseconds
uint64_t time_get_uptime_ms(void) {
    return system_uptime_ms;
}

// Get system uptime in seconds
uint64_t time_get_uptime_sec(void) {
    return system_uptime_ms / 1000;
}

// Get current date/time
datetime_t time_get_datetime(void) {
    return current_datetime;
}

// Format time as HH:MM:SS string
void time_format_time(char* buffer, size_t bufsize) {
    if (!buffer || bufsize < 9) return;  // Need at least "HH:MM:SS\0"
    
    // Format: HH:MM:SS
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

// Format date as YYYY-MM-DD string
void time_format_date(char* buffer, size_t bufsize) {
    if (!buffer || bufsize < 11) return;  // Need at least "YYYY-MM-DD\0"
    
    // Format: YYYY-MM-DD
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

// Format datetime as "YYYY-MM-DD HH:MM:SS" string
void time_format_datetime(char* buffer, size_t bufsize) {
    if (!buffer || bufsize < 20) return;  // Need at least "YYYY-MM-DD HH:MM:SS\0"
    
    char date[11];
    char time[9];
    
    time_format_date(date, sizeof(date));
    time_format_time(time, sizeof(time));
    
    // Combine: "YYYY-MM-DD HH:MM:SS"
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

// Format uptime as "Xd Xh Xm Xs" string
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

// Print current date and time with color
void time_print_datetime(bool show_uptime) {
    char datetime_str[20];
    time_format_datetime(datetime_str, sizeof(datetime_str));
    
    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("[");
    print_str(datetime_str);
    print_str("]");
    
    if (show_uptime) {
        char uptime_str[32];
        time_format_uptime(uptime_str, sizeof(uptime_str));
        
        print_str(" [Uptime: ");
        print_str(uptime_str);
        print_str("]");
    }
    
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
}