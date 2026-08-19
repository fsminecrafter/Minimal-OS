#include "graphics.h"
#include "x86_64/gpu.h"
#include "x86_64/allocator.h"
#include "panic.h"
#include "serial.h"
#include "string.h"

//8x8 font at dhepper/font8x8 - github
#include "font8x8_basic.h"

// Global state
static gpu_device_t* g_gpu = NULL;
bool graphics_safety_mode = true;
static terminal_t g_terminal = {0};
static font_t g_current_font = {0};

static int last_x = -1;
static int last_y = -1;

// ===========================================
// Terminal character buffer for scroll support
// Stores one char + fg color per cell for redraw-on-scroll.
// TERM_MAX_COLS and TERM_MAX_ROWS come from graphics.h.
// ===========================================

typedef struct {
    char    ch;
    uint8_t r, g, b;
} term_cell_t;

// Use the header's TERM_MAX_ROWS / TERM_MAX_COLS values.
// Declared as a flat array in BSS — no heap allocation needed.
static term_cell_t g_term_buf[TERM_MAX_ROWS][TERM_MAX_COLS];

void term_buf_clear_row(uint32_t row) {
    if (row >= TERM_MAX_ROWS) return;
    for (uint32_t c = 0; c < TERM_MAX_COLS; c++) {
        g_term_buf[row][c].ch = ' ';
        g_term_buf[row][c].r  = g_terminal.fg_color.r;
        g_term_buf[row][c].g  = g_terminal.fg_color.g;
        g_term_buf[row][c].b  = g_terminal.fg_color.b;
    }
}

void term_buf_set(uint32_t col, uint32_t row, char ch, color_t fg) {
    if (col >= TERM_MAX_COLS || row >= TERM_MAX_ROWS) return;
    g_term_buf[row][col].ch = ch;
    g_term_buf[row][col].r  = fg.r;
    g_term_buf[row][col].g  = fg.g;
    g_term_buf[row][col].b  = fg.b;
}

// Helper macros
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS(x) ((x) < 0 ? -(x) : (x))

// ... [Previous graphics.c code for basic drawing] ...
// [Include all the functions from the previous graphics.c]

// ===========================================
// BMP Image Loading
// ===========================================

#pragma pack(push, 1)
typedef struct {
    uint16_t signature;      // 'BM'
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t data_offset;
} bmp_header_t;

typedef struct {
    uint32_t header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t image_size;
    int32_t  x_pixels_per_meter;
    int32_t  y_pixels_per_meter;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_header_t;
#pragma pack(pop)

bool graphics_load_bmp(bmp_image_t* image, const uint8_t* bmp_data, uint32_t data_size) {
    if (!image || !bmp_data || data_size < sizeof(bmp_header_t) + sizeof(bmp_info_header_t)) {
        serial_write_str("graphics: BMP data too small or NULL\n");
        return false;
    }

    const bmp_header_t* header = (const bmp_header_t*)bmp_data;
    if (header->signature != 0x4D42) {
        serial_write_str("graphics: Invalid BMP signature\n");
        return false;
    }

    // data_offset must be within the buffer
    if (header->data_offset >= data_size) {
        serial_write_str("graphics: BMP data_offset out of range\n");
        return false;
    }

    const bmp_info_header_t* info = (const bmp_info_header_t*)(bmp_data + sizeof(bmp_header_t));

    // Only support 24bpp and 32bpp uncompressed
    if (info->compression != 0) {
        serial_write_str("graphics: BMP compression not supported\n");
        return false;
    }
    if (info->bits_per_pixel != 24 && info->bits_per_pixel != 32) {
        serial_write_str("graphics: BMP bpp not supported (need 24 or 32)\n");
        return false;
    }

    uint32_t width  = (uint32_t)ABS(info->width);
    uint32_t height = (uint32_t)ABS(info->height);

    if (width == 0 || height == 0 || width > 8192 || height > 8192) {
        serial_write_str("graphics: BMP dimensions out of range\n");
        return false;
    }

    // Check pixel buffer fits in the file
    uint32_t bytes_per_pixel = info->bits_per_pixel / 8;
    uint32_t row_size = ((info->bits_per_pixel * width + 31) / 32) * 4;
    // Overflow-safe check: row_size * height <= data_size - data_offset
    if (height > 0 && row_size > (data_size - header->data_offset) / height) {
        serial_write_str("graphics: BMP pixel data overflows buffer\n");
        return false;
    }

    serial_write_str("Loading BMP: ");
    serial_write_dec(width);
    serial_write_str("x");
    serial_write_dec(height);
    serial_write_str(" @ ");
    serial_write_dec(info->bits_per_pixel);
    serial_write_str(" bpp\n");

    // Use alloc_unzeroed — we fill every pixel below
    image->pixels = (uint32_t*)alloc_unzeroed(width * height * sizeof(uint32_t));
    if (!image->pixels) {
        serial_write_str("graphics: Failed to allocate BMP pixel buffer\n");
        return false;
    }

    image->width  = width;
    image->height = height;

    const uint8_t* pixel_data = bmp_data + header->data_offset;
    bool bottom_up = info->height > 0;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t src_y = bottom_up ? (height - 1 - y) : y;
        const uint8_t* row = pixel_data + src_y * row_size;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel;
            if (info->bits_per_pixel == 24) {
                uint8_t b = row[x * 3 + 0];
                uint8_t g = row[x * 3 + 1];
                uint8_t r = row[x * 3 + 2];
                pixel = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            } else {
                uint8_t b = row[x * 4 + 0];
                uint8_t g = row[x * 4 + 1];
                uint8_t r = row[x * 4 + 2];
                uint8_t a = row[x * 4 + 3];
                pixel = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
            image->pixels[y * width + x] = pixel;
        }
    }

    image->loaded = true;
    serial_write_str("BMP loaded successfully\n");
    return true;
}

void graphics_draw_image(bmp_image_t* image, int32_t center_x, int32_t center_y) {
    if (!image || !image->loaded) return;
    
    int32_t x = center_x - (int32_t)(image->width / 2);
    int32_t y = center_y - (int32_t)(image->height / 2);
    
    graphics_draw_image_tl(image, x, y);
}

void graphics_draw_image_tl(bmp_image_t* image, int32_t x, int32_t y) {
    if (!image || !image->loaded) return;
    
    for (uint32_t py = 0; py < image->height; py++) {
        for (uint32_t px = 0; px < image->width; px++) {
            uint32_t pixel = image->pixels[py * image->width + px];
            uint8_t a = (pixel >> 24) & 0xFF;
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;
            
            // Skip fully transparent pixels
            if (a > 0) {
                graphics_write_pixel(x + px, y + py, r, g, b);
            }
        }
    }
}

void graphics_draw_image_scaled(bmp_image_t* image, int32_t center_x, int32_t center_y,
                                uint32_t new_width, uint32_t new_height) {
    if (!image || !image->loaded) return;
    
    int32_t start_x = center_x - (int32_t)(new_width / 2);
    int32_t start_y = center_y - (int32_t)(new_height / 2);
    
    // Nearest-neighbor scaling
    for (uint32_t y = 0; y < new_height; y++) {
        for (uint32_t x = 0; x < new_width; x++) {
            uint32_t src_x = (x * image->width) / new_width;
            uint32_t src_y = (y * image->height) / new_height;
            
            uint32_t pixel = image->pixels[src_y * image->width + src_x];
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;
            
            graphics_write_pixel(start_x + x, start_y + y, r, g, b);
        }
    }
}

void graphics_free_image(bmp_image_t* image) {
    if (image && image->pixels) {
        free_mem(image->pixels);
        image->pixels = NULL;
        image->loaded = false;
    }
}

void graphics_terminal_putchar(char c, uint32_t col, uint32_t row, color_t fg, color_t bg) {
    if (c < 32 || c > 126)
        c = '?';

    uint32_t char_index = (uint8_t)c;

    uint32_t px = col * g_terminal.char_width;
    uint32_t py = row * g_terminal.char_height;

    for (uint32_t y = 0; y < 8 && y < g_terminal.char_height; y++) {
        uint8_t row_data = font8x8_basic[char_index][y];

        for (uint32_t x = 0; x < 8 && x < g_terminal.char_width; x++) {
            bool pixel_set = (row_data & (1 << x)) != 0;

            color_t color = pixel_set ? fg : bg;

            graphics_write_pixel_c(px + x, py + y, color);
        }
    }
}

char terminal_get_char(uint32_t col, uint32_t row) {
    if (col >= TERM_MAX_COLS || row >= TERM_MAX_ROWS) {
        return ' ';
    }

    return g_term_buf[row][col].ch;
}

color_t terminal_get_color(uint32_t col, uint32_t row) {
    if (col >= TERM_MAX_COLS || row >= TERM_MAX_ROWS) {
        return g_terminal.fg_color;
    }

    term_cell_t* cell = &g_term_buf[row][col];
    return (color_t){cell->r, cell->g, cell->b, 0xFF};
}

void erase_cursor(int cx, int cy) {
    if (cx < 0 || cy < 0) return;

    char c = terminal_get_char(cx, cy);
    color_t fg = terminal_get_color(cx, cy);

    graphics_terminal_putchar(
        c,
        cx,
        cy,
        fg,
        g_terminal.bg_color
    );
}

void terminalUpdateCursor() {
    // Erase old cursor
    if (last_x != -1 && last_y != -1) {
        erase_cursor(last_x, last_y);
    }

    if (!g_terminal.cursor_visible) {
        last_x = -1;
        last_y = -1;
        return;
    }

    uint32_t px = g_terminal.cursor_x * g_terminal.char_width;
    uint32_t py = g_terminal.cursor_y * g_terminal.char_height;

    uint32_t cy = py + g_terminal.char_height - 2;

    for (uint32_t x = 0; x < g_terminal.char_width; x++) {
        graphics_write_pixel_c(px + x, cy, g_terminal.fg_color);
        graphics_write_pixel_c(px + x, cy + 1, g_terminal.fg_color);
    }

    last_x = g_terminal.cursor_x;
    last_y = g_terminal.cursor_y;
}

// ===========================================
// Terminal Functions
// ===========================================

void graphics_set_resolution(uint32_t cols, uint32_t rows) {
    if (!g_gpu) {
        serial_write_str("graphics_set_resolution: GPU not initialized\n");
        return;
    }

    // Fixed font size
    g_terminal.char_width = 8;
    g_terminal.char_height = 8;

    // Maximum characters that fit
    uint32_t max_cols = g_gpu->width / g_terminal.char_width;
    uint32_t max_rows = g_gpu->height / g_terminal.char_height;

    // Clamp requested size
    if (cols > max_cols)
        cols = max_cols;

    if (rows > max_rows)
        rows = max_rows;

    g_terminal.cols = cols;
    g_terminal.rows = rows;

    g_terminal.cursor_x = 0;
    g_terminal.cursor_y = 0;

    g_terminal.fg_color = COLOR_WHITE;
    g_terminal.bg_color = COLOR_BLACK;
    g_terminal.cursor_visible = true;

    serial_write_str("Terminal resolution set to ");
    serial_write_dec(cols);
    serial_write_str("x");
    serial_write_dec(rows);
    serial_write_str(" (8x8 pixels per char, max ");
    serial_write_dec(max_cols);
    serial_write_str("x");
    serial_write_dec(max_rows);
    serial_write_str(")\n");

    graphics_terminal_clear();
}

terminal_t* graphics_get_terminal(void) {
    return &g_terminal;
}

void graphics_terminal_clear(void) {
    graphics_clear_c(g_terminal.bg_color);
    g_terminal.cursor_x = 0;
    g_terminal.cursor_y = 0;
    // Clear character buffer
    for (uint32_t row = 0; row < TERM_MAX_ROWS; row++) {
        term_buf_clear_row(row);
    }
}

void graphics_terminal_newline(void) {
    g_terminal.cursor_x = 0;
    g_terminal.cursor_y++;
    
    if (g_terminal.cursor_y >= g_terminal.rows) {
        graphics_terminal_scroll();
    }
}

void graphics_terminal_scroll(void) {
    if (!g_gpu) return;

    uint32_t rows = g_terminal.rows;
    uint32_t cols = g_terminal.cols;

    // Shift the character buffer up by one row
    for (uint32_t row = 1; row < rows && row < TERM_MAX_ROWS; row++) {
        for (uint32_t col = 0; col < cols && col < TERM_MAX_COLS; col++) {
            g_term_buf[row - 1][col] = g_term_buf[row][col];
        }
    }

    // Clear the last row in the buffer
    if (rows > 0) term_buf_clear_row(rows - 1);

    // Redraw all rows from the character buffer
    for (uint32_t row = 0; row < rows && row < TERM_MAX_ROWS; row++) {
        for (uint32_t col = 0; col < cols && col < TERM_MAX_COLS; col++) {
            term_cell_t* cell = &g_term_buf[row][col];
            color_t fg = {cell->r, cell->g, cell->b, 0xFF};
            graphics_terminal_putchar(cell->ch, col, row, fg, g_terminal.bg_color);
        }
    }

    g_terminal.cursor_y = rows - 1;
}

void graphics_terminal_set_cursor(uint32_t x, uint32_t y) {
    g_terminal.cursor_x = MIN(x, g_terminal.cols - 1);
    g_terminal.cursor_y = MIN(y, g_terminal.rows - 1);
}

void graphics_terminal_get_cursor(uint32_t* x, uint32_t* y) {
    if (x) *x = g_terminal.cursor_x;
    if (y) *y = g_terminal.cursor_y;
}

void graphics_terminal_set_color(color_t fg, color_t bg) {
    g_terminal.fg_color = fg;
    g_terminal.bg_color = bg;
}

void graphics_write_textr_char(char c) {
    if (c == '\n') {
        graphics_terminal_newline();
        return;
    }
    
    if (c == '\r') {
        g_terminal.cursor_x = 0;
        return;
    }
    
    if (c == '\t') {
        g_terminal.cursor_x = (g_terminal.cursor_x + 4) & ~3;
        if (g_terminal.cursor_x >= g_terminal.cols) {
            graphics_terminal_newline();
        }
        return;
    }
    
    // Record in character buffer for scroll support
    term_buf_set(g_terminal.cursor_x, g_terminal.cursor_y, c, g_terminal.fg_color);

    // Draw character
    graphics_terminal_putchar(c, g_terminal.cursor_x, g_terminal.cursor_y, g_terminal.fg_color, g_terminal.bg_color);
    
    // Advance cursor
    g_terminal.cursor_x++;
    if (g_terminal.cursor_x >= g_terminal.cols) {
        graphics_terminal_newline();
    }
}

void graphics_write_textr(const char* text) {
    if (!text) return;
    
    while (*text) {
        graphics_write_textr_char(*text);
        text++;
    }
}

void graphics_write_textr_dec(int64_t value) {
    char buffer[32];
    int_to_str(value, buffer);
    graphics_write_textr(buffer);
}

void graphics_write_textr_udec(uint64_t value) {
    char buffer[32];
    uint_to_str(value, buffer);
    graphics_write_textr(buffer);
}

void graphics_write_textr_hex(uint64_t value) {
    char buffer[32];
    buffer[0] = '0';
    buffer[1] = 'x';
    hex_to_str(value, buffer + 2);
    graphics_write_textr(buffer);
}

void graphics_write_textr_hex64(uint64_t value) {
    graphics_write_textr("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (value >> i) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
        graphics_write_textr_char(c);
    }
}

void graphics_write_textr_bin(uint64_t value) {
    graphics_write_textr("0b");
    for (int i = 63; i >= 0; i--) {
        graphics_write_textr_char((value & (1ULL << i)) ? '1' : '0');
    }
}

// ===========================================
// Font Functions (TTF support would require FreeType)
// ===========================================

bool graphics_set_font(const char* font_path, uint32_t size, font_style_t style) {
    // For now, use built-in font
    // TTF support would require integrating FreeType library
    serial_write_str("graphics: TTF fonts not yet supported, using built-in 8x16 font\n");
    g_current_font.size = size;
    g_current_font.style = style;
    g_current_font.letter_spacing = 1;
    g_current_font.line_spacing = 2;
    return true;
}

void graphics_set_font_size(uint32_t size) {
    g_current_font.size = size;
}

void graphics_set_font_style(font_style_t style) {
    g_current_font.style = style;
}

void graphics_set_text_spacing(uint32_t letter_spacing, uint32_t line_spacing) {
    g_current_font.letter_spacing = letter_spacing;
    g_current_font.line_spacing = line_spacing;
}

void graphics_write_text(const char* text, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b) {
    // Simple text rendering using built-in font
    int32_t current_x = x;
    int32_t current_y = y;
    color_t fg = graphics_rgb(r, g, b);
    color_t bg = COLOR_BLACK;
    
    while (*text) {
        if (*text == '\n') {
            current_x = x;
            current_y += 16 + g_current_font.line_spacing;
            text++;
            continue;
        }
        
        if (*text == '\t') {
            current_x += 32;  // 4 character widths
            text++;
            continue;
        }
        
        // Draw character using built-in font
        uint32_t char_index = (uint8_t)(*text);
        if (char_index >= 128)
            char_index = 0;   // fallback to first glyph
        for (uint32_t py = 0; py < 8; py++) {
            uint8_t row_data = font8x8_basic[char_index][py];
            for (uint32_t px = 0; px < 8; px++) {
                if (row_data & (1 << px)) {
                    graphics_write_pixel_c(current_x + px, current_y + py, fg);
                }
            }
        }
        
        current_x += 8 + g_current_font.letter_spacing;
        text++;
    }
}

void graphics_write_text_c(const char* text, int32_t x, int32_t y, color_t color) {
    graphics_write_text(text, x, y, color.r, color.g, color.b);
}

void graphics_measure_text(const char* text, uint32_t* width, uint32_t* height) {
    uint32_t max_width = 0;
    uint32_t current_width = 0;
    uint32_t lines = 1;
    
    while (*text) {
        if (*text == '\n') {
            if (current_width > max_width) max_width = current_width;
            current_width = 0;
            lines++;
        } else {
            current_width += 8 + g_current_font.letter_spacing;
        }
        text++;
    }
    
    if (current_width > max_width) max_width = current_width;
    
    if (width) *width = max_width;
    if (height) *height = lines * (16 + g_current_font.line_spacing);
}
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

// Validate coordinates: returns false and sets x/y to -1 if GPU not ready,
// otherwise clamps to screen bounds and returns true.
// Drawing functions should skip the pixel when this returns false.
bool graphics_validate_coords(int32_t* x, int32_t* y) {
    if (!g_gpu) return false;

    int32_t width  = (int32_t)g_gpu->width;
    int32_t height = (int32_t)g_gpu->height;

    // Silently discard completely off-screen pixels
    if (*x < 0 || *x >= width || *y < 0 || *y >= height) {
        if (graphics_safety_mode) {
            // In safety mode: skip the pixel entirely (don't draw, don't panic)
            return false;
        }
        // Clamp to nearest valid pixel
        if (*x < 0)       *x = 0;
        if (*x >= width)  *x = width  - 1;
        if (*y < 0)       *y = 0;
        if (*y >= height) *y = height - 1;
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
        serial_write_str("graphics_clear: GPU not initialized\n");
        return;
    }
    
    uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
    gpu_clear(g_gpu, color);
}

void graphics_clear_c(color_t color) {
    graphics_clear(color.r, color.g, color.b);
}