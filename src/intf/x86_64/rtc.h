#ifndef RTC_H
#define RTC_H

#include <stdint.h>

// Initialize RTC
void rtc_init(void);

// Read current time/date values
uint8_t rtc_seconds(void);
uint8_t rtc_minutes(void);
uint8_t rtc_hours(void);    // 24-hour format (0-23)
uint8_t rtc_day(void);      // Day of month (1-31)
uint8_t rtc_month(void);    // Month (1-12)
uint16_t rtc_year(void);    // Full year (e.g., 2026)

#endif // RTC_H