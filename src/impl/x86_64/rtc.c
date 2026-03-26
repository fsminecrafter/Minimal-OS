#include "x86_64/rtc.h"
#include "x86_64/port.h"
#include "time.h"
#include <stdint.h>

uint8_t rtc_seconds() {
    return rtc_read_reg(RTC_SECONDS);
}

uint8_t rtc_minutes() {
    return rtc_read_reg(RTC_MINUTES);
}

void rtc_get_datetime(datetime_t* dt) {
    dt->second = rtc_seconds();
    dt->minute = rtc_minutes();
    dt->hour   = rtc_hours();
    dt->day    = rtc_day();
    dt->month  = rtc_month();
    dt->year   = rtc_year();
}

uint8_t rtc_hours() {
    uint8_t hour = rtc_read_reg(RTC_HOURS);
    uint8_t status_b = rtc_read_reg(RTC_STATUS_B);
    
    // Handle 12-hour format if bit 1 is not set
    if (!(status_b & 0x02)) {
        // 12-hour format
        int pm = hour & 0x80;
        hour &= 0x7F;
        
        if (!(status_b & 0x04)) {
            // BCD mode
            hour = bcd_to_binary(hour);
        }
        
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }
    
    return hour;
}

uint8_t rtc_day() {
    return rtc_read_reg(RTC_DAY);
}

uint8_t rtc_month() {
    return rtc_read_reg(RTC_MONTH);
}

uint16_t rtc_year() {
    uint8_t year = rtc_read_reg(RTC_YEAR);
    uint8_t century = rtc_read_reg(RTC_CENTURY);
    
    // If century register is 0 or invalid, assume 21st century
    if (century == 0 || century == 0xFF) {
        century = 20;
    } else {
        uint8_t status_b = rtc_read_reg(RTC_STATUS_B);
        if (!(status_b & 0x04)) {
            // BCD mode
            century = bcd_to_binary(century);
        }
    }
    
    return (uint16_t)(century * 100 + year);
}

// Initialize RTC (optional - set 24-hour mode if needed)
void rtc_init() {
    // Disable NMI and read status register B
    port_outb(CMOS_ADDRESS, 0x8B);
    uint8_t status_b = port_inb(CMOS_DATA);
    
    // Set 24-hour mode (bit 1)
    status_b |= 0x02;
    
    // Write back
    port_outb(CMOS_ADDRESS, 0x8B);
    port_outb(CMOS_DATA, status_b);
}

void rtc_write_seconds(uint8_t s) {
    rtc_write_reg(RTC_SECONDS, s);
}

void rtc_write_minutes(uint8_t m) {
    rtc_write_reg(RTC_MINUTES, m);
}

void rtc_write_hours(uint8_t h) {
    rtc_write_reg(RTC_HOURS, h);
}

void rtc_write_day(uint8_t d) {
    rtc_write_reg(RTC_DAY, d);
}

void rtc_write_month(uint8_t m) {
    rtc_write_reg(RTC_MONTH, m);
}

void rtc_write_year(uint16_t year) {

    uint8_t yr = year % 100;
    uint8_t century = year / 100;

    rtc_write_reg(RTC_YEAR, yr);
    rtc_write_reg(RTC_CENTURY, century);
}