#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>

// Color structure for convenience
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;  // Alpha (usually 0xFF for opaque)
} color_t;

// Common colors
#define COLOR_BLACK       ((color_t){0x00, 0x00, 0x00, 0xFF})
#define COLOR_WHITE       ((color_t){0xFF, 0xFF, 0xFF, 0xFF})
#define COLOR_RED         ((color_t){0xFF, 0x00, 0x00, 0xFF})
#define COLOR_GREEN       ((color_t){0x00, 0xFF, 0x00, 0xFF})
#define COLOR_BLUE        ((color_t){0x00, 0x00, 0xFF, 0xFF})
#define COLOR_YELLOW      ((color_t){0xFF, 0xFF, 0x00, 0xFF})
#define COLOR_CYAN        ((color_t){0x00, 0xFF, 0xFF, 0xFF})
#define COLOR_MAGENTA     ((color_t){0xFF, 0x00, 0xFF, 0xFF})
#define COLOR_ORANGE      ((color_t){0xFF, 0xA5, 0x00, 0xFF})
#define COLOR_PURPLE      ((color_t){0x80, 0x00, 0x80, 0xFF})
#define COLOR_GRAY        ((color_t){0x80, 0x80, 0x80, 0xFF})
#define COLOR_LIGHT_GRAY  ((color_t){0xC0, 0xC0, 0xC0, 0xFF})
#define COLOR_DARK_GRAY   ((color_t){0x40, 0x40, 0x40, 0xFF})

// Graphics configuration
extern bool graphics_safety_mode;  // true = panic on out-of-bounds, false = clamp to nearest

// Initialize graphics system (must be called after GPU initialization)
void graphics_init(void);

// Get framebuffer dimensions
uint32_t graphics_get_width(void);
uint32_t graphics_get_height(void);

// Core drawing functions
void graphics_write_pixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b);
void graphics_write_pixel_c(int32_t x, int32_t y, color_t color);

void graphics_write_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t r, uint8_t g, uint8_t b);
void graphics_write_line_c(int32_t x1, int32_t y1, int32_t x2, int32_t y2, color_t color);

void graphics_write_circle(int32_t center_x, int32_t center_y, uint32_t radius, uint8_t r, uint8_t g, uint8_t b);
void graphics_write_circle_c(int32_t center_x, int32_t center_y, uint32_t radius, color_t color);
void graphics_fill_circle(int32_t center_x, int32_t center_y, uint32_t radius, uint8_t r, uint8_t g, uint8_t b);
void graphics_fill_circle_c(int32_t center_x, int32_t center_y, uint32_t radius, color_t color);

void graphics_write_rectangle(int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b);
void graphics_write_rectangle_c(int32_t x, int32_t y, uint32_t width, uint32_t height, color_t color);
void graphics_fill_rectangle(int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b);
void graphics_fill_rectangle_c(int32_t x, int32_t y, uint32_t width, uint32_t height, color_t color);

// Additional drawing primitives
void graphics_write_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, uint8_t r, uint8_t g, uint8_t b);
void graphics_write_triangle_c(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, color_t color);
void graphics_fill_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, uint8_t r, uint8_t g, uint8_t b);
void graphics_fill_triangle_c(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, color_t color);

void graphics_write_ellipse(int32_t center_x, int32_t center_y, uint32_t radius_x, uint32_t radius_y, uint8_t r, uint8_t g, uint8_t b);
void graphics_write_ellipse_c(int32_t center_x, int32_t center_y, uint32_t radius_x, uint32_t radius_y, color_t color);

// Screen operations
void graphics_clear(uint8_t r, uint8_t g, uint8_t b);
void graphics_clear_c(color_t color);

// Utility functions
color_t graphics_rgb(uint8_t r, uint8_t g, uint8_t b);
color_t graphics_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint32_t graphics_color_to_u32(color_t color);

// Coordinate validation (internal use)
bool graphics_validate_coords(int32_t* x, int32_t* y);

#endif // GRAPHICS_H