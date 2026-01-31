#include "x86_64/rtc.h"
#include "x86_64/port.h"
#include <stdint.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

// CMOS registers
#define RTC_SECONDS       0x00
#define RTC_MINUTES       0x02
#define RTC_HOURS         0x04
#define RTC_DAY           0x07
#define RTC_MONTH         0x08
#define RTC_YEAR          0x09
#define RTC_CENTURY       0x32
#define RTC_STATUS_A      0x0A
#define RTC_STATUS_B      0x0B

// Read from CMOS register
static uint8_t cmos_read(uint8_t reg) {
    port_outb(CMOS_ADDRESS, reg);
    return port_inb(CMOS_DATA);
}

// Check if RTC is updating
static int rtc_is_updating() {
    port_outb(CMOS_ADDRESS, RTC_STATUS_A);
    return (port_inb(CMOS_DATA) & 0x80);
}

// Convert BCD to binary if needed
static uint8_t bcd_to_binary(uint8_t bcd) {
    return ((bcd & 0xF0) >> 4) * 10 + (bcd & 0x0F);
}

// Read RTC value with BCD conversion check
static uint8_t rtc_read_reg(uint8_t reg) {
    // Wait for update to complete
    while (rtc_is_updating());
    
    uint8_t value = cmos_read(reg);
    
    // Check if RTC is in BCD mode
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    if (!(status_b & 0x04)) {
        // BCD mode - convert to binary
        value = bcd_to_binary(value);
    }
    
    return value;
}

uint8_t rtc_seconds() {
    return rtc_read_reg(RTC_SECONDS);
}

uint8_t rtc_minutes() {
    return rtc_read_reg(RTC_MINUTES);
}

uint8_t rtc_hours() {
    uint8_t hour = rtc_read_reg(RTC_HOURS);
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    
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
        uint8_t status_b = cmos_read(RTC_STATUS_B);
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