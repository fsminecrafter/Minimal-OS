#ifndef USER_GRAPHICS_H
#define USER_GRAPHICS_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

static inline uint32_t mos_graphics_get_width(void) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_GET_WIDTH };
    return (uint32_t)mos_graphics(&request);
}

static inline uint32_t mos_graphics_get_height(void) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_GET_HEIGHT };
    return (uint32_t)mos_graphics(&request);
}

static inline void mos_graphics_clear(uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_CLEAR,
                                           .args = { r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_pixel(int32_t x, int32_t y,
                                      uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_PIXEL,
                                           .args = { x, y, r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_line(int32_t x1, int32_t y1, int32_t x2,
                                     int32_t y2, uint8_t r, uint8_t g,
                                     uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_LINE,
                                           .args = { x1, y1, x2, y2, r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_rectangle(int32_t x, int32_t y, uint32_t width,
                                          uint32_t height, uint8_t r, uint8_t g,
                                          uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_RECTANGLE,
                                           .args = { x, y, width, height, r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_fill_rectangle(int32_t x, int32_t y,
                                               uint32_t width, uint32_t height,
                                               uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_FILL_RECTANGLE,
                                           .args = { x, y, width, height, r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_triangle(int32_t x1, int32_t y1, int32_t x2,
                                         int32_t y2, int32_t x3, int32_t y3,
                                         uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_TRIANGLE,
                                           .args = { x1, y1, x2, y2, x3, y3,
                                                     r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_fill_triangle(int32_t x1, int32_t y1,
                                              int32_t x2, int32_t y2,
                                              int32_t x3, int32_t y3,
                                              uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_FILL_TRIANGLE,
                                           .args = { x1, y1, x2, y2, x3, y3,
                                                     r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_circle(int32_t x, int32_t y, uint32_t radius,
                                        uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_CIRCLE,
                                           .args = { x, y, radius, r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_fill_circle(int32_t x, int32_t y,
                                            uint32_t radius, uint8_t r,
                                            uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_FILL_CIRCLE,
                                           .args = { x, y, radius, r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_ellipse(int32_t x, int32_t y, uint32_t radius_x,
                                        uint32_t radius_y, uint8_t r, uint8_t g,
                                        uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_ELLIPSE,
                                           .args = { x, y, radius_x, radius_y,
                                                     r, g, b } };
    mos_graphics(&request);
}

static inline void mos_graphics_text(const char* text, int32_t x, int32_t y,
                                     uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_TEXT,
                                           .args = { x, y, r, g, b },
                                           .text = text };
    mos_graphics(&request);
}

static inline void mos_graphics_set_resolution(uint32_t columns,
                                               uint32_t rows) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_SET_RESOLUTION,
                                           .args = { columns, rows } };
    mos_graphics(&request);
}

#endif
