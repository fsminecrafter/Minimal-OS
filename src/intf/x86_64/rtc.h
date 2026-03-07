#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include "time.h"

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

// Initialize RTC
void rtc_init(void);

// Read current time/date values
uint8_t rtc_seconds(void);
uint8_t rtc_minutes(void);
uint8_t rtc_hours(void);    // 24-hour format (0-23)
uint8_t rtc_day(void);      // Day of month (1-31)
uint8_t rtc_month(void);    // Month (1-12)
uint16_t rtc_year(void);    // Full year (e.g., 2026)
void rtc_get_datetime(datetime_t* dt);
void rtc_write_year(uint16_t year);
void rtc_write_month(uint8_t m);
void rtc_write_day(uint8_t d);
void rtc_write_hours(uint8_t h);
void rtc_write_minutes(uint8_t m);
void rtc_write_seconds(uint8_t s);

#endif // RTC_H