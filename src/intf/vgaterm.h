#ifndef VGATERM_H
#define VGATERM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "graphics.h"

/*
 * VGA Terminal Enhancements
 * 
 * Features:
 * - Color codes in strings: "/cr255g128b64/"
 * - Yes/No prompts with callbacks
 * - Multiple choice prompts
 * - Loading bars (progress and spinner)
 * - Status indicators
 * - Box drawing
 * - Animated elements
 */

// ===========================================
// COLOR CODES
// ===========================================

/**
 * Parse and print string with embedded color codes
 * 
 * Color codes: /cr255g128b64/
 *   r = red (0-255)
 *   g = green (0-255)
 *   b = blue (0-255)
 * 
 * Example:
 *   vgaterm_print("/cr255g0b0/ERROR: /cr255g255b255/File not found");
 *   // Prints "ERROR: " in red, "File not found" in white
 */
void vgaterm_print(const char* text);
void vgaterm_println(const char* text);

/**
 * Print with predefined colors
 */
void vgaterm_print_error(const char* text);
void vgaterm_print_warning(const char* text);
void vgaterm_print_success(const char* text);
void vgaterm_print_info(const char* text);

// ===========================================
// PROMPTS
// ===========================================

typedef void (*prompt_callback_t)(void);

/**
 * Yes/No prompts
 * 
 * Usage:
 *   if (vgaterm_ask_yn("Delete file?", true)) {
 *       // User said yes
 *   }
 */
bool vgaterm_ask_yn(const char* question, bool default_yes);  // Y/n
bool vgaterm_ask_ny(const char* question, bool default_no);   // y/N

/**
 * Multiple choice prompt
 * 
 * Usage:
 *   const char* options[] = {"Save", "Discard", "Cancel"};
 *   int choice = vgaterm_ask_choice("What to do?", options, 3, 0);
 */
int vgaterm_ask_choice(const char* question, const char** options, 
                       int option_count, int default_choice);

/**
 * Multiple choice with callbacks
 * 
 * Usage:
 *   const char* opts[] = {"Yes", "No", "Cancel"};
 *   prompt_callback_t cbs[] = {do_yes, do_no, do_cancel};
 *   vgaterm_ask_with_callbacks("Continue?", opts, cbs, 3);
 */
void vgaterm_ask_with_callbacks(const char* question, const char** options,
                                 prompt_callback_t* callbacks, int count);

/**
 * Password/hidden input
 */
void vgaterm_ask_password(const char* prompt, char* buffer, size_t max_len);

/**
 * Text input with prompt
 */
void vgaterm_ask_input(const char* prompt, char* buffer, size_t max_len);

// ===========================================
// LOADING BARS
// ===========================================

typedef enum {
    LOADBAR_STYLE_PROGRESS,   // [###  ] 60%
    LOADBAR_STYLE_SPINNER,    // [*** ] rotating
    LOADBAR_STYLE_DOTS,       // [... ] bouncing
    LOADBAR_STYLE_SERVICE     // [ OK ] / [FAIL] / [WAIT]
} loadbar_style_t;

typedef enum {
    SERVICE_STARTING,
    SERVICE_OK,
    SERVICE_FAIL,
    SERVICE_WARN
} service_state_t;

typedef struct {
    uint16_t row;              // Row position
    uint16_t width;            // Width in characters
    bool moveable;             // Scroll with terminal?
    bool visible;              // Is visible?
    
    loadbar_style_t style;
    
    // Progress bar
    uint8_t progress;          // 0-100 for progress bar
    
    // Spinner/dots
    uint8_t frame;             // Animation frame
    
    // Service status
    service_state_t service_state;
    char label[64];            // Label text
    
    // Colors
    color_t bar_color;
    color_t bg_color;
    color_t text_color;
} vgaterm_loadbar_t;

/**
 * Create loading bar
 * 
 * Usage:
 *   vgaterm_loadbar_t* bar = vgaterm_loadbar_create(2, 20, true, LOADBAR_STYLE_PROGRESS);
 *   vgaterm_loadbar_set_progress(bar, 50);  // 50%
 *   // Later...
 *   vgaterm_loadbar_destroy(bar);
 */
vgaterm_loadbar_t* vgaterm_loadbar_create(uint16_t row, uint16_t width, 
                                           bool moveable, loadbar_style_t style);
void vgaterm_loadbar_destroy(vgaterm_loadbar_t* bar);

/**
 * Update loading bar
 */
void vgaterm_loadbar_set_progress(vgaterm_loadbar_t* bar, uint8_t progress);
void vgaterm_loadbar_set_service_state(vgaterm_loadbar_t* bar, 
                                       service_state_t state, const char* label);
void vgaterm_loadbar_tick(vgaterm_loadbar_t* bar);  // Advance animation frame
void vgaterm_loadbar_render(vgaterm_loadbar_t* bar);

// ===========================================
// BOX DRAWING
// ===========================================

typedef enum {
    BOX_SINGLE,    // ┌─┐│└┘
    BOX_DOUBLE,    // ╔═╗║╚╝
    BOX_ROUNDED,   // ╭─╮│╰╯
    BOX_ASCII      // +--+||
} box_style_t;

/**
 * Draw a box
 * 
 * Usage:
 *   vgaterm_draw_box(5, 5, 40, 10, BOX_SINGLE, "Title");
 */
void vgaterm_draw_box(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      box_style_t style, const char* title);

/**
 * Clear area inside box
 */
void vgaterm_clear_box(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

// ===========================================
// STATUS INDICATORS
// ===========================================

typedef enum {
    STATUS_INFO,     // [i] blue
    STATUS_OK,       // [✓] green
    STATUS_WARN,     // [!] yellow
    STATUS_ERROR,    // [✗] red
    STATUS_BUSY,     // [⋯] gray, animated
} status_type_t;

/**
 * Print status indicator with message
 * 
 * Usage:
 *   vgaterm_status(STATUS_OK, "File saved");
 *   vgaterm_status(STATUS_ERROR, "Connection failed");
 */
void vgaterm_status(status_type_t type, const char* message);

// ===========================================
// PROGRESS INDICATORS
// ===========================================

/**
 * Show progress with percentage
 * 
 * Usage:
 *   vgaterm_progress("Downloading", 45, 100);
 *   // Output: Downloading... [45/100] 45%
 */
void vgaterm_progress(const char* label, uint32_t current, uint32_t total);

/**
 * Show indeterminate progress (spinner)
 */
void vgaterm_spinner(const char* label);  // Will rotate on each call

// ===========================================
// MENUS
// ===========================================

typedef struct {
    const char* label;
    prompt_callback_t callback;
    bool enabled;
} menu_item_t;

/**
 * Display menu and handle selection
 * 
 * Usage:
 *   menu_item_t items[] = {
 *       {"New File", do_new, true},
 *       {"Open File", do_open, true},
 *       {"Exit", do_exit, true}
 *   };
 *   vgaterm_menu("Main Menu", items, 3);
 */
void vgaterm_menu(const char* title, menu_item_t* items, int count);

// ===========================================
// TABLES
// ===========================================

typedef struct {
    const char** headers;
    const char*** data;
    int column_count;
    int row_count;
    int* column_widths;  // NULL = auto
} table_t;

/**
 * Display a table
 * 
 * Usage:
 *   const char* headers[] = {"Name", "Size", "Date"};
 *   const char* row1[] = {"file.txt", "1.2KB", "2026-03-26"};
 *   const char* row2[] = {"data.bin", "512B", "2026-03-25"};
 *   const char** rows[] = {row1, row2};
 *   
 *   table_t table = {headers, rows, 3, 2, NULL};
 *   vgaterm_table(&table);
 */
void vgaterm_table(table_t* table);

// ===========================================
// ANIMATIONS
// ===========================================

/**
 * Animate a sequence of frames
 */
typedef struct {
    const char** frames;
    int frame_count;
    int current_frame;
    uint16_t x, y;
    uint32_t delay_ms;
    uint32_t last_update;
} animation_t;

animation_t* vgaterm_animation_create(uint16_t x, uint16_t y, 
                                      const char** frames, int count,
                                      uint32_t delay_ms);
void vgaterm_animation_update(animation_t* anim);
void vgaterm_animation_destroy(animation_t* anim);

// ===========================================
// UTILITY FUNCTIONS
// ===========================================

/**
 * Center text on row
 */
void vgaterm_print_centered(uint16_t row, const char* text);

/**
 * Print at specific position
 */
void vgaterm_print_at(uint16_t col, uint16_t row, const char* text);

/**
 * Save/restore cursor position
 */
void vgaterm_cursor_save(void);
void vgaterm_cursor_restore(void);

/**
 * Clear line
 */
void vgaterm_clear_line(uint16_t row);

/**
 * Horizontal line
 */
void vgaterm_hline(uint16_t row, uint16_t col, uint16_t length, char ch);

/**
 * Wait for keypress
 */
char vgaterm_wait_key(void);

/**
 * Beep/bell
 */
void vgaterm_beep(void);

// ===========================================
// PREDEFINED COLOR SCHEMES
// ===========================================

extern const char* VGATERM_COLOR_ERROR;
extern const char* VGATERM_COLOR_WARNING;
extern const char* VGATERM_COLOR_SUCCESS;
extern const char* VGATERM_COLOR_INFO;
extern const char* VGATERM_COLOR_PROMPT;
extern const char* VGATERM_COLOR_RESET;

// ===========================================
// INITIALIZATION
// ===========================================

/**
 * Initialize VGA terminal enhancements
 */
void vgaterm_init(void);

#endif // VGATERM_H