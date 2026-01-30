#include "print.h"
#include "x86_64/rtc.h"
#include "bool.h"

uint16_t prev_seconds = 0;

uint16_t currentsystemtime_sec(bool debug) {

    uint16_t seconds = rtc_seconds();
            
    if (seconds != prev_seconds) {
        if (debug == 1) {
            print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
            print_str("\nSeconds: ");
            print_uint64_dec(seconds);
            }
        
        prev_seconds = seconds;
    }
    return seconds;
}