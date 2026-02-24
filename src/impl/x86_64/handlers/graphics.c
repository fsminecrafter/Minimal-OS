#include "graphics.h"
#include "x86_64/gpu.h"
#include "panic.h"
#include "serial.h"

// Global state
static gpu_device_t* g_gpu = NULL;
bool graphics_safety_mode = true;  // Default: panic on out-of-bounds

// Helper macros
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS(x) ((x) < 0 ? -(x) : (x))

// Initialize graphics system
void graphics_init(void) {
    // GPU should already be initialized by gpu_init()
    // This just prepares the graphics subsystem
    serial_write_str("Graphics subsystem initialized\n");
    serial_write_str("Safety mode: ");
    serial_write_str(graphics_safety_mode ? "PANIC" : "CLAMP");
    serial_write_str("\n");
}

// Set GPU device (called from main after GPU initialization)
void graphics_set_gpu(gpu_device_t* gpu) {
    g_gpu = gpu;
}

// Get framebuffer dimensions
uint32_t graphics_get_width(void) {
    return g_gpu ? g_gpu->width : 0;
}

uint32_t graphics_get_height(void) {
    return g_gpu ? g_gpu->height : 0;
}

// Validate and fix coordinates
bool graphics_validate_coords(int32_t* x, int32_t* y) {
    if (!g_gpu) {
        PANIC("graphics: GPU not initialized");
        return false;
    }
    
    int32_t width = (int32_t)g_gpu->width;
    int32_t height = (int32_t)g_gpu->height;
    
    // Check if out of bounds
    if (*x < 0 || *x >= width || *y < 0 || *y >= height) {
        if (graphics_safety_mode) {
            serial_write_str("graphics: Out of bounds pixel at (");
            serial_write_dec(*x);
            serial_write_str(", ");
            serial_write_dec(*y);
            serial_write_str(") - framebuffer is ");
            serial_write_dec(width);
            serial_write_str("x");
            serial_write_dec(height);
            serial_write_str("\n");
            PANIC("graphics: Pixel out of bounds (safety mode enabled)");
            return false;
        } else {
            // Clamp to nearest valid pixel
            if (*x < 0) *x = 0;
            if (*x >= width) *x = width - 1;
            if (*y < 0) *y = 0;
            if (*y >= height) *y = height - 1;
        }
    }
    
    return true;
}

// Color utilities
color_t graphics_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (color_t){r, g, b, 0xFF};
}

color_t graphics_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (color_t){r, g, b, a};
}

uint32_t graphics_color_to_u32(color_t color) {
    // ARGB format: 0xAARRGGBB
    return (color.a << 24) | (color.r << 16) | (color.g << 8) | color.b;
}

// Core pixel writing
void graphics_write_pixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (!graphics_validate_coords(&x, &y)) return;
    
    uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
    gpu_put_pixel(g_gpu, (uint32_t)x, (uint32_t)y, color);
}

void graphics_write_pixel_c(int32_t x, int32_t y, color_t color) {
    graphics_write_pixel(x, y, color.r, color.g, color.b);
}

// Line drawing using Bresenham's algorithm
void graphics_write_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t r, uint8_t g, uint8_t b) {
    int32_t dx = ABS(x2 - x1);
    int32_t dy = ABS(y2 - y1);
    int32_t sx = (x1 < x2) ? 1 : -1;
    int32_t sy = (y1 < y2) ? 1 : -1;
    int32_t err = dx - dy;
    
    int32_t x = x1;
    int32_t y = y1;
    
    while (1) {
        graphics_write_pixel(x, y, r, g, b);
        
        if (x == x2 && y == y2) break;
        
        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

void graphics_write_line_c(int32_t x1, int32_t y1, int32_t x2, int32_t y2, color_t color) {
    graphics_write_line(x1, y1, x2, y2, color.r, color.g, color.b);
}

// Circle drawing using midpoint circle algorithm
void graphics_write_circle(int32_t center_x, int32_t center_y, uint32_t radius, uint8_t r, uint8_t g, uint8_t b) {
    int32_t x = 0;
    int32_t y = (int32_t)radius;
    int32_t d = 3 - 2 * (int32_t)radius;
    
    while (x <= y) {
        // Draw 8 octants
        graphics_write_pixel(center_x + x, center_y + y, r, g, b);
        graphics_write_pixel(center_x - x, center_y + y, r, g, b);
        graphics_write_pixel(center_x + x, center_y - y, r, g, b);
        graphics_write_pixel(center_x - x, center_y - y, r, g, b);
        graphics_write_pixel(center_x + y, center_y + x, r, g, b);
        graphics_write_pixel(center_x - y, center_y + x, r, g, b);
        graphics_write_pixel(center_x + y, center_y - x, r, g, b);
        graphics_write_pixel(center_x - y, center_y - x, r, g, b);
        
        if (d < 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

void graphics_write_circle_c(int32_t center_x, int32_t center_y, uint32_t radius, color_t color) {
    graphics_write_circle(center_x, center_y, radius, color.r, color.g, color.b);
}

// Filled circle
void graphics_fill_circle(int32_t center_x, int32_t center_y, uint32_t radius, uint8_t r, uint8_t g, uint8_t b) {
    int32_t rad = (int32_t)radius;
    for (int32_t y = -rad; y <= rad; y++) {
        for (int32_t x = -rad; x <= rad; x++) {
            if (x * x + y * y <= rad * rad) {
                graphics_write_pixel(center_x + x, center_y + y, r, g, b);
            }
        }
    }
}

void graphics_fill_circle_c(int32_t center_x, int32_t center_y, uint32_t radius, color_t color) {
    graphics_fill_circle(center_x, center_y, radius, color.r, color.g, color.b);
}

// Rectangle outline
void graphics_write_rectangle(int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    // Top edge
    graphics_write_line(x, y, x + width - 1, y, r, g, b);
    // Bottom edge
    graphics_write_line(x, y + height - 1, x + width - 1, y + height - 1, r, g, b);
    // Left edge
    graphics_write_line(x, y, x, y + height - 1, r, g, b);
    // Right edge
    graphics_write_line(x + width - 1, y, x + width - 1, y + height - 1, r, g, b);
}

void graphics_write_rectangle_c(int32_t x, int32_t y, uint32_t width, uint32_t height, color_t color) {
    graphics_write_rectangle(x, y, width, height, color.r, color.g, color.b);
}

// Filled rectangle
void graphics_fill_rectangle(int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t dy = 0; dy < height; dy++) {
        for (uint32_t dx = 0; dx < width; dx++) {
            graphics_write_pixel(x + dx, y + dy, r, g, b);
        }
    }
}

void graphics_fill_rectangle_c(int32_t x, int32_t y, uint32_t width, uint32_t height, color_t color) {
    graphics_fill_rectangle(x, y, width, height, color.r, color.g, color.b);
}

// Triangle outline
void graphics_write_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, uint8_t r, uint8_t g, uint8_t b) {
    graphics_write_line(x1, y1, x2, y2, r, g, b);
    graphics_write_line(x2, y2, x3, y3, r, g, b);
    graphics_write_line(x3, y3, x1, y1, r, g, b);
}

void graphics_write_triangle_c(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, color_t color) {
    graphics_write_triangle(x1, y1, x2, y2, x3, y3, color.r, color.g, color.b);
}

// Helper for triangle fill
static void fill_triangle_scanline(int32_t y, int32_t x1, int32_t x2, uint8_t r, uint8_t g, uint8_t b) {
    if (x1 > x2) {
        int32_t tmp = x1;
        x1 = x2;
        x2 = tmp;
    }
    for (int32_t x = x1; x <= x2; x++) {
        graphics_write_pixel(x, y, r, g, b);
    }
}

// Filled triangle (scanline algorithm)
void graphics_fill_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, uint8_t r, uint8_t g, uint8_t b) {
    // Sort vertices by y-coordinate (y1 <= y2 <= y3)
    if (y1 > y2) { int32_t tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
    if (y1 > y3) { int32_t tx = x1, ty = y1; x1 = x3; y1 = y3; x3 = tx; y3 = ty; }
    if (y2 > y3) { int32_t tx = x2, ty = y2; x2 = x3; y2 = y3; x3 = tx; y3 = ty; }
    
    // Render upper triangle
    int32_t dy12 = y2 - y1;
    int32_t dy13 = y3 - y1;
    
    for (int32_t y = y1; y <= y2; y++) {
        int32_t xa = dy12 != 0 ? x1 + (x2 - x1) * (y - y1) / dy12 : x1;
        int32_t xb = dy13 != 0 ? x1 + (x3 - x1) * (y - y1) / dy13 : x1;
        fill_triangle_scanline(y, xa, xb, r, g, b);
    }
    
    // Render lower triangle
    int32_t dy23 = y3 - y2;
    for (int32_t y = y2 + 1; y <= y3; y++) {
        int32_t xa = dy23 != 0 ? x2 + (x3 - x2) * (y - y2) / dy23 : x2;
        int32_t xb = dy13 != 0 ? x1 + (x3 - x1) * (y - y1) / dy13 : x1;
        fill_triangle_scanline(y, xa, xb, r, g, b);
    }
}

void graphics_fill_triangle_c(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, color_t color) {
    graphics_fill_triangle(x1, y1, x2, y2, x3, y3, color.r, color.g, color.b);
}

// Ellipse using midpoint algorithm
void graphics_write_ellipse(int32_t center_x, int32_t center_y, uint32_t radius_x, uint32_t radius_y, uint8_t r, uint8_t g, uint8_t b) {
    int32_t rx = (int32_t)radius_x;
    int32_t ry = (int32_t)radius_y;
    int32_t rx2 = rx * rx;
    int32_t ry2 = ry * ry;
    int32_t two_rx2 = 2 * rx2;
    int32_t two_ry2 = 2 * ry2;
    
    int32_t x = 0;
    int32_t y = ry;
    int32_t px = 0;
    int32_t py = two_rx2 * y;
    
    // Region 1
    int32_t p = ry2 - (rx2 * ry) + (rx2 / 4);
    while (px < py) {
        graphics_write_pixel(center_x + x, center_y + y, r, g, b);
        graphics_write_pixel(center_x - x, center_y + y, r, g, b);
        graphics_write_pixel(center_x + x, center_y - y, r, g, b);
        graphics_write_pixel(center_x - x, center_y - y, r, g, b);
        
        x++;
        px += two_ry2;
        if (p < 0) {
            p += ry2 + px;
        } else {
            y--;
            py -= two_rx2;
            p += ry2 + px - py;
        }
    }
    
    // Region 2
    p = ry2 * (x * x + x) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
    while (y >= 0) {
        graphics_write_pixel(center_x + x, center_y + y, r, g, b);
        graphics_write_pixel(center_x - x, center_y + y, r, g, b);
        graphics_write_pixel(center_x + x, center_y - y, r, g, b);
        graphics_write_pixel(center_x - x, center_y - y, r, g, b);
        
        y--;
        py -= two_rx2;
        if (p > 0) {
            p += rx2 - py;
        } else {
            x++;
            px += two_ry2;
            p += rx2 - py + px;
        }
    }
}

void graphics_write_ellipse_c(int32_t center_x, int32_t center_y, uint32_t radius_x, uint32_t radius_y, color_t color) {
    graphics_write_ellipse(center_x, center_y, radius_x, radius_y, color.r, color.g, color.b);
}

// Clear screen
void graphics_clear(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_gpu) {
        PANIC("graphics: GPU not initialized");
        return;
    }
    
    uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
    gpu_clear(g_gpu, color);
}

void graphics_clear_c(color_t color) {
    graphics_clear(color.r, color.g, color.b);
}