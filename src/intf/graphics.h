#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>

#include "x86_64/gpu.h"

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

#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 128

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

// ===========================================
// BMP Image Loading and Rendering
// ===========================================

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t* pixels;  // ARGB format
    bool loaded;
} bmp_image_t;

// Load BMP from memory buffer (e.g., embedded in kernel)
bool graphics_load_bmp(bmp_image_t* image, const uint8_t* bmp_data, uint32_t data_size);

// Load BMP from file path (if filesystem available)
bool graphics_load_bmp_file(bmp_image_t* image, const char* filepath);

// Draw BMP image (x, y is CENTER of image)
void graphics_draw_image(bmp_image_t* image, int32_t center_x, int32_t center_y);

// Draw BMP image at top-left corner
void graphics_draw_image_tl(bmp_image_t* image, int32_t x, int32_t y);

// Draw BMP image scaled
void graphics_draw_image_scaled(bmp_image_t* image, int32_t center_x, int32_t center_y, 
                                uint32_t new_width, uint32_t new_height);

// Free BMP image memory
void graphics_free_image(bmp_image_t* image);

// ===========================================
// Font and Text Rendering
// ===========================================

typedef enum {
    FONT_STYLE_NORMAL = 0,
    FONT_STYLE_BOLD = 1,
    FONT_STYLE_ITALIC = 2,
    FONT_STYLE_BOLD_ITALIC = 3
} font_style_t;

typedef struct {
    const char* name;
    uint32_t size;
    font_style_t style;
    uint32_t letter_spacing;
    uint32_t line_spacing;
    bool loaded;
    void* font_data;  // Internal font data
} font_t;

//Sets the current active gpu
void graphics_set_gpu(gpu_device_t* gpu);

// Set current font (loads TTF if not already loaded)
bool graphics_set_font(const char* font_path, uint32_t size, font_style_t style);

// Set font size only (keeps current font)
void graphics_set_font_size(uint32_t size);

// Set font style only
void graphics_set_font_style(font_style_t style);

// Set text spacing
void graphics_set_text_spacing(uint32_t letter_spacing, uint32_t line_spacing);

// Write text at position (supports \n, \t, \r)
void graphics_write_text(const char* text, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b);
void graphics_write_text_c(const char* text, int32_t x, int32_t y, color_t color);

// Get text dimensions (for layout calculations)
void graphics_measure_text(const char* text, uint32_t* width, uint32_t* height);

// ===========================================
// Terminal-like Text Mode (Resolution-based)
// ===========================================

typedef struct {
    uint32_t cols;           // Number of columns (characters per line)
    uint32_t rows;           // Number of rows (lines)
    uint32_t cursor_x;       // Current cursor column
    uint32_t cursor_y;       // Current cursor row
    uint32_t char_width;     // Width of each character cell in pixels
    uint32_t char_height;    // Height of each character cell in pixels
    color_t fg_color;        // Foreground text color
    color_t bg_color;        // Background color
    bool cursor_visible;     // Show/hide cursor
} terminal_t;

// Set terminal resolution (columns x rows)
// Automatically calculates char_width and char_height to fill screen
void graphics_set_resolution(uint32_t cols, uint32_t rows);

// Get current terminal
terminal_t* graphics_get_terminal(void);

// Terminal output functions
void graphics_write_textr(const char* text);  // Write with automatic \n handling
void graphics_write_textr_dec(int64_t value);
void graphics_write_textr_udec(uint64_t value);
void graphics_write_textr_hex(uint64_t value);
void graphics_write_textr_hex64(uint64_t value);
void graphics_write_textr_bin(uint64_t value);
void graphics_write_textr_char(char c);

// Terminal control
void graphics_terminal_clear(void);
void graphics_terminal_newline(void);
void graphics_terminal_set_cursor(uint32_t x, uint32_t y);
void graphics_terminal_get_cursor(uint32_t* x, uint32_t* y);
void graphics_terminal_set_color(color_t fg, color_t bg);
void graphics_terminal_scroll(void);
void terminalUpdateCursor();
// Built-in bitmap font (8x16 default)
void graphics_terminal_putchar(char c, uint32_t col, uint32_t row, color_t fg, color_t bg);

#endif // GRAPHICS_H