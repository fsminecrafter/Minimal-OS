#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * HID mouse reports in "report protocol". Boot protocol (what BIOSes use)
 * only has buttons, X and Y: most real mice leave the wheel out of it.
 * The report descriptor says where each field sits, so the wheel and
 * high-resolution X/Y can be read. No kernel dependencies (host tests).
 */

typedef struct {
    uint16_t bit;       // offset in the report, after the report ID byte
    uint8_t size;       // bits, 0 = absent
    bool is_signed;
} hid_field_t;

typedef struct {
    bool valid;
    uint8_t report_id;  // 0 = the device uses no report IDs
    hid_field_t x, y, wheel;
    uint16_t button_bit;
    uint8_t button_count;
} hid_mouse_layout_t;

// Finds buttons, X, Y and the wheel of the first mouse report in a report
// descriptor. Returns false when there is no relative X/Y.
bool hid_mouse_parse(const uint8_t* desc, uint32_t length, hid_mouse_layout_t* out);

// Reads one report (as received, report ID byte included). Returns false
// when it is another report. Movement is clamped to +-32767.
bool hid_mouse_decode(const hid_mouse_layout_t* layout, const uint8_t* report,
                      uint32_t length, uint8_t* buttons, int32_t* dx, int32_t* dy,
                      int32_t* wheel);
