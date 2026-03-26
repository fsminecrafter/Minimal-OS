#include "vgaterm.h"
#include "graphics.h"
#include "string.h"
#include "serial.h"
#include "time.h"
#include "x86_64/allocator.h"
#include "keyboard/usbkeyboard.h"

// ===========================================
// PREDEFINED COLORS
// ===========================================

const char* VGATERM_COLOR_ERROR   = "/cr255g0b0/";        // Red
const char* VGATERM_COLOR_WARNING = "/cr255g255b0/";      // Yellow
const char* VGATERM_COLOR_SUCCESS = "/cr0g255b0/";        // Green
const char* VGATERM_COLOR_INFO    = "/cr0g255b255/";      // Cyan
const char* VGATERM_COLOR_PROMPT  = "/cr255g255b255/";    // White
const char* VGATERM_COLOR_RESET   = "/cr255g255b255/";    // White

// ===========================================
// STATE
// ===========================================

static uint16_t saved_cursor_x = 0;
static uint16_t saved_cursor_y = 0;
static uint8_t spinner_frame = 0;

// ===========================================
// INITIALIZATION
// ===========================================

void vgaterm_init(void) {
    serial_write_str("VGATerm: Initializing terminal enhancements\n");
    spinner_frame = 0;
}

// ===========================================
// COLOR PARSING
// ===========================================

// Parse color code like "/cr255g128b64/"
static bool parse_color_code(const char* str, color_t* color, int* consumed) {
    // Format: /cr###g###b###/
    if (str[0] != '/' || str[1] != 'c') {
        return false;
    }
    
    int r = -1, g = -1, b = -1;
    const char* p = str + 2;
    
    // Parse r, g, b
    while (*p && *p != '/') {
        if (*p == 'r') {
            p++;
            r = 0;
            while (*p >= '0' && *p <= '9') {
                r = r * 10 + (*p - '0');
                p++;
            }
        }
        else if (*p == 'g') {
            p++;
            g = 0;
            while (*p >= '0' && *p <= '9') {
                g = g * 10 + (*p - '0');
                p++;
            }
        }
        else if (*p == 'b') {
            p++;
            b = 0;
            while (*p >= '0' && *p <= '9') {
                b = b * 10 + (*p - '0');
                p++;
            }
        }
        else {
            p++;
        }
    }
    
    if (*p == '/' && r >= 0 && g >= 0 && b >= 0) {
        color->r = (r > 255) ? 255 : r;
        color->g = (g > 255) ? 255 : g;
        color->b = (b > 255) ? 255 : b;
        *consumed = (p + 1) - str;
        return true;
    }
    
    return false;
}

void vgaterm_print(const char* text) {
    if (!text) return;
    
    const char* p = text;
    color_t current_color = {255, 255, 255};
    
    while (*p) {
        // Check for color code
        int consumed = 0;
        color_t new_color;
        
        if (parse_color_code(p, &new_color, &consumed)) {
            current_color = new_color;
            graphics_terminal_set_color(current_color, (color_t){0, 0, 0});
            p += consumed;
            continue;
        }
        
        // Regular character
        graphics_write_textr_char(*p);
        p++;
    }
}

void vgaterm_println(const char* text) {
    vgaterm_print(text);
    graphics_write_textr("\n");
}

// ===========================================
// PREDEFINED COLOR FUNCTIONS
// ===========================================

void vgaterm_print_error(const char* text) {
    vgaterm_print(VGATERM_COLOR_ERROR);
    vgaterm_print(text);
    vgaterm_print(VGATERM_COLOR_RESET);
}

void vgaterm_print_warning(const char* text) {
    vgaterm_print(VGATERM_COLOR_WARNING);
    vgaterm_print(text);
    vgaterm_print(VGATERM_COLOR_RESET);
}

void vgaterm_print_success(const char* text) {
    vgaterm_print(VGATERM_COLOR_SUCCESS);
    vgaterm_print(text);
    vgaterm_print(VGATERM_COLOR_RESET);
}

void vgaterm_print_info(const char* text) {
    vgaterm_print(VGATERM_COLOR_INFO);
    vgaterm_print(text);
    vgaterm_print(VGATERM_COLOR_RESET);
}

// ===========================================
// PROMPTS
// ===========================================

// Wait for Y/N key press
static bool wait_yes_no(bool default_yes) {
    while (true) {
        char key = vgaterm_wait_key();
        
        if (key == 'y' || key == 'Y') {
            vgaterm_print_success("Yes\n");
            return true;
        }
        if (key == 'n' || key == 'N') {
            vgaterm_print_error("No\n");
            return false;
        }
        if (key == '\n' || key == '\r') {
            if (default_yes) {
                vgaterm_print_success("Yes\n");
                return true;
            } else {
                vgaterm_print_error("No\n");
                return false;
            }
        }
    }
}

bool vgaterm_ask_yn(const char* question, bool default_yes) {
    vgaterm_print(VGATERM_COLOR_PROMPT);
    vgaterm_print(question);
    vgaterm_print(VGATERM_COLOR_INFO);
    
    if (default_yes) {
        vgaterm_print(" (Y/n): ");
    } else {
        vgaterm_print(" (y/N): ");
    }
    
    vgaterm_print(VGATERM_COLOR_RESET);
    
    return wait_yes_no(default_yes);
}

bool vgaterm_ask_ny(const char* question, bool default_no) {
    return !vgaterm_ask_yn(question, !default_no);
}

int vgaterm_ask_choice(const char* question, const char** options, 
                       int option_count, int default_choice) {
    vgaterm_print(VGATERM_COLOR_PROMPT);
    vgaterm_print(question);
    vgaterm_print("\n");
    
    for (int i = 0; i < option_count; i++) {
        vgaterm_print("  ");
        
        if (i == default_choice) {
            vgaterm_print(VGATERM_COLOR_SUCCESS);
            vgaterm_print("[");
        } else {
            vgaterm_print(" ");
        }
        
        char num[4];
        num[0] = '0' + (i + 1);
        num[1] = '\0';
        vgaterm_print(num);
        
        if (i == default_choice) {
            vgaterm_print("]");
            vgaterm_print(VGATERM_COLOR_RESET);
        }
        
        vgaterm_print(" ");
        vgaterm_print(options[i]);
        vgaterm_print("\n");
    }
    
    vgaterm_print(VGATERM_COLOR_INFO);
    vgaterm_print("Select (1-");
    char max[4];
    max[0] = '0' + option_count;
    max[1] = '\0';
    vgaterm_print(max);
    vgaterm_print("): ");
    vgaterm_print(VGATERM_COLOR_RESET);
    
    while (true) {
        char key = vgaterm_wait_key();
        
        if (key >= '1' && key <= '9') {
            int choice = key - '1';
            if (choice < option_count) {
                vgaterm_print(&key);
                vgaterm_print("\n");
                return choice;
            }
        }
        if (key == '\n' || key == '\r') {
            vgaterm_print_success("Default\n");
            return default_choice;
        }
    }
}

void vgaterm_ask_with_callbacks(const char* question, const char** options,
                                 prompt_callback_t* callbacks, int count) {
    int choice = vgaterm_ask_choice(question, options, count, 0);
    
    if (choice >= 0 && choice < count && callbacks[choice]) {
        callbacks[choice]();
    }
}

void vgaterm_ask_password(const char* prompt, char* buffer, size_t max_len) {
    vgaterm_print(VGATERM_COLOR_PROMPT);
    vgaterm_print(prompt);
    vgaterm_print(": ");
    vgaterm_print(VGATERM_COLOR_RESET);
    
    size_t pos = 0;
    while (pos < max_len - 1) {
        char key = vgaterm_wait_key();
        
        if (key == '\n' || key == '\r') {
            buffer[pos] = '\0';
            vgaterm_print("\n");
            return;
        }
        if (key == '\b' || key == 127) {  // Backspace
            if (pos > 0) {
                pos--;
                graphics_write_textr_char('\b');
                graphics_write_textr_char(' ');
                graphics_write_textr_char('\b');
            }
        }
        else if (key >= 32 && key <= 126) {
            buffer[pos++] = key;
            graphics_write_textr_char('*');
        }
    }
    
    buffer[pos] = '\0';
    vgaterm_print("\n");
}

void vgaterm_ask_input(const char* prompt, char* buffer, size_t max_len) {
    vgaterm_print(VGATERM_COLOR_PROMPT);
    vgaterm_print(prompt);
    vgaterm_print(": ");
    vgaterm_print(VGATERM_COLOR_RESET);
    
    size_t pos = 0;
    while (pos < max_len - 1) {
        char key = vgaterm_wait_key();
        
        if (key == '\n' || key == '\r') {
            buffer[pos] = '\0';
            vgaterm_print("\n");
            return;
        }
        if (key == '\b' || key == 127) {
            if (pos > 0) {
                pos--;
                graphics_write_textr_char('\b');
                graphics_write_textr_char(' ');
                graphics_write_textr_char('\b');
            }
        }
        else if (key >= 32 && key <= 126) {
            buffer[pos++] = key;
            graphics_write_textr_char(key);
        }
    }
    
    buffer[pos] = '\0';
    vgaterm_print("\n");
}


// ===========================================
// LOADING BARS
// ===========================================

vgaterm_loadbar_t* vgaterm_loadbar_create(uint16_t row, uint16_t width, 
                                           bool moveable, loadbar_style_t style) {
    vgaterm_loadbar_t* bar = (vgaterm_loadbar_t*)alloc(sizeof(vgaterm_loadbar_t));
    if (!bar) return NULL;
    
    bar->row = row;
    bar->width = width;
    bar->moveable = moveable;
    bar->visible = true;
    bar->style = style;
    bar->progress = 0;
    bar->frame = 0;
    bar->service_state = SERVICE_STARTING;
    bar->label[0] = '\0';
    
    // Default colors
    bar->bar_color = (color_t){0, 255, 0};    // Green
    bar->bg_color = (color_t){64, 64, 64};    // Dark gray
    bar->text_color = (color_t){255, 255, 255}; // White
    
    return bar;
}

void vgaterm_loadbar_destroy(vgaterm_loadbar_t* bar) {
    if (bar) {
        free_mem(bar);
    }
}

void vgaterm_loadbar_set_progress(vgaterm_loadbar_t* bar, uint8_t progress) {
    if (!bar) return;
    if (progress > 100) progress = 100;
    bar->progress = progress;
}

void vgaterm_loadbar_set_service_state(vgaterm_loadbar_t* bar, 
                                       service_state_t state, const char* label) {
    if (!bar) return;
    bar->service_state = state;
    if (label) {
        strncpy(bar->label, label, sizeof(bar->label) - 1);
        bar->label[sizeof(bar->label) - 1] = '\0';
    }
}

void vgaterm_loadbar_tick(vgaterm_loadbar_t* bar) {
    if (!bar) return;
    bar->frame++;
}

void vgaterm_loadbar_render(vgaterm_loadbar_t* bar) {
    if (!bar || !bar->visible) return;
    
    // Save cursor
    terminal_t* term = graphics_get_terminal();
    uint16_t old_x = term->cursor_x;
    uint16_t old_y = term->cursor_y;
    
    // Move to bar position
    term->cursor_x = 0;
    term->cursor_y = bar->row;
    
    switch (bar->style) {
        case LOADBAR_STYLE_PROGRESS: {
            // [###  ] 60%
            graphics_write_textr_char('[');
            
            uint16_t filled = (bar->progress * (bar->width - 7)) / 100;
            
            // Draw filled portion
            graphics_terminal_set_color(bar->bar_color, bar->bg_color);
            for (uint16_t i = 0; i < filled; i++) {
                graphics_write_textr_char('#');
            }
            
            // Draw empty portion
            graphics_terminal_set_color(bar->bg_color, (color_t){0,0,0});
            for (uint16_t i = filled; i < (bar->width - 7); i++) {
                graphics_write_textr_char(' ');
            }
            
            graphics_terminal_set_color(bar->text_color, (color_t){0,0,0});
            graphics_write_textr_char(']');
            graphics_write_textr_char(' ');
            
            // Print percentage
            char pct[8];
            snprintf(pct, sizeof(pct), "%3d%%", bar->progress);
            graphics_write_textr(pct);
            break;
        }
        
        case LOADBAR_STYLE_SPINNER: {
            // [*** ] rotating
            const char* spinner_frames[] = {
                "[*   ]",
                "[ *  ]",
                "[  * ]",
                "[   *]",
                "[  * ]",
                "[ *  ]"
            };
            
            uint8_t frame_idx = bar->frame % 6;
            graphics_write_textr(spinner_frames[frame_idx]);
            break;
        }
        
        case LOADBAR_STYLE_DOTS: {
            // [... ] bouncing
            const char* dot_frames[] = {
                "[.   ]",
                "[ .  ]",
                "[  . ]",
                "[   .]",
                "[  . ]",
                "[ .  ]"
            };
            
            uint8_t frame_idx = bar->frame % 6;
            graphics_write_textr(dot_frames[frame_idx]);
            break;
        }
        
        case LOADBAR_STYLE_SERVICE: {
            // [ OK ] / [FAIL] / [WAIT]
            switch (bar->service_state) {
                case SERVICE_STARTING:
                    graphics_terminal_set_color((color_t){255,255,0}, (color_t){0,0,0});
                    graphics_write_textr("[WAIT]");
                    break;
                    
                case SERVICE_OK:
                    graphics_terminal_set_color((color_t){0,255,0}, (color_t){0,0,0});
                    graphics_write_textr("[ OK ]");
                    break;
                    
                case SERVICE_FAIL:
                    graphics_terminal_set_color((color_t){255,0,0}, (color_t){0,0,0});
                    graphics_write_textr("[FAIL]");
                    break;
                    
                case SERVICE_WARN:
                    graphics_terminal_set_color((color_t){255,255,0}, (color_t){0,0,0});
                    graphics_write_textr("[WARN]");
                    break;
            }
            
            graphics_terminal_set_color(bar->text_color, (color_t){0,0,0});
            graphics_write_textr(" ");
            graphics_write_textr(bar->label);
            break;
        }
    }
    
    // Restore cursor
    term->cursor_x = old_x;
    term->cursor_y = old_y;
}

// ===========================================
// STATUS INDICATORS
// ===========================================

void vgaterm_status(status_type_t type, const char* message) {
    switch (type) {
        case STATUS_INFO:
            vgaterm_print("/cr0g255b255/[i]/cr255g255b255/ ");
            break;
        case STATUS_OK:
            vgaterm_print("/cr0g255b0/[✓]/cr255g255b255/ ");
            break;
        case STATUS_WARN:
            vgaterm_print("/cr255g255b0/[!]/cr255g255b255/ ");
            break;
        case STATUS_ERROR:
            vgaterm_print("/cr255g0b0/[✗]/cr255g255b255/ ");
            break;
        case STATUS_BUSY:
            vgaterm_print("/cr128g128b128/[⋯]/cr255g255b255/ ");
            break;
    }
    
    vgaterm_print(message);
    vgaterm_print("\n");
}

// ===========================================
// PROGRESS INDICATORS
// ===========================================

void vgaterm_progress(const char* label, uint32_t current, uint32_t total) {
    vgaterm_print(label);
    vgaterm_print("... [");
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%u/%u", current, total);
    vgaterm_print(buf);
    vgaterm_print("] ");
    
    uint8_t percent = (total > 0) ? ((current * 100) / total) : 0;
    snprintf(buf, sizeof(buf), "%u%%", percent);
    
    if (percent < 50) {
        vgaterm_print_error(buf);
    } else if (percent < 100) {
        vgaterm_print_warning(buf);
    } else {
        vgaterm_print_success(buf);
    }
    
    vgaterm_print("\n");
}

void vgaterm_spinner(const char* label) {
    const char* frames[] = {"|", "/", "-", "\\"};
    
    vgaterm_print(label);
    vgaterm_print(" ");
    vgaterm_print(frames[spinner_frame % 4]);
    vgaterm_print("\r");  // Return to start of line
    
    spinner_frame++;
}

// ===========================================
// BOX DRAWING
// ===========================================

void vgaterm_draw_box(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      box_style_t style, const char* title) {
    terminal_t* term = graphics_get_terminal();
    uint16_t old_x = term->cursor_x;
    uint16_t old_y = term->cursor_y;
    
    const char* tl, *tr, *bl, *br, *h, *v;
    
    switch (style) {
        case BOX_SINGLE:
            tl = "┌"; tr = "┐"; bl = "└"; br = "┘"; h = "─"; v = "│";
            break;
        case BOX_DOUBLE:
            tl = "╔"; tr = "╗"; bl = "╚"; br = "╝"; h = "═"; v = "║";
            break;
        case BOX_ROUNDED:
            tl = "╭"; tr = "╮"; bl = "╰"; br = "╯"; h = "─"; v = "│";
            break;
        case BOX_ASCII:
        default:
            tl = "+"; tr = "+"; bl = "+"; br = "+"; h = "-"; v = "|";
            break;
    }
    
    // Top line
    term->cursor_x = x;
    term->cursor_y = y;
    graphics_write_textr(tl);
    
    // Title if provided
    if (title && *title) {
        graphics_write_textr(" ");
        graphics_write_textr(title);
        graphics_write_textr(" ");
        
        uint16_t title_len = strlen(title) + 2;
        for (uint16_t i = title_len; i < width - 2; i++) {
            graphics_write_textr(h);
        }
    } else {
        for (uint16_t i = 0; i < width - 2; i++) {
            graphics_write_textr(h);
        }
    }
    
    graphics_write_textr(tr);
    
    // Sides
    for (uint16_t row = 1; row < height - 1; row++) {
        term->cursor_x = x;
        term->cursor_y = y + row;
        graphics_write_textr(v);
        
        term->cursor_x = x + width - 1;
        graphics_write_textr(v);
    }
    
    // Bottom line
    term->cursor_x = x;
    term->cursor_y = y + height - 1;
    graphics_write_textr(bl);
    
    for (uint16_t i = 0; i < width - 2; i++) {
        graphics_write_textr(h);
    }
    
    graphics_write_textr(br);
    
    // Restore cursor
    term->cursor_x = old_x;
    term->cursor_y = old_y;
}

void vgaterm_clear_box(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    terminal_t* term = graphics_get_terminal();
    uint16_t old_x = term->cursor_x;
    uint16_t old_y = term->cursor_y;
    
    for (uint16_t row = 0; row < height; row++) {
        term->cursor_x = x;
        term->cursor_y = y + row;
        
        for (uint16_t col = 0; col < width; col++) {
            graphics_write_textr_char(' ');
        }
    }
    
    term->cursor_x = old_x;
    term->cursor_y = old_y;
}

// ===========================================
// MENU
// ===========================================

void vgaterm_menu(const char* title, menu_item_t* items, int count) {
    vgaterm_draw_box(5, 3, 50, count + 4, BOX_SINGLE, title);
    
    terminal_t* term = graphics_get_terminal();
    
    for (int i = 0; i < count; i++) {
        term->cursor_x = 7;
        term->cursor_y = 4 + i;
        
        if (!items[i].enabled) {
            vgaterm_print("/cr128g128b128/");  // Gray for disabled
        }
        
        char num[8];
        snprintf(num, sizeof(num), "%d. ", i + 1);
        vgaterm_print(num);
        vgaterm_print(items[i].label);
        
        if (!items[i].enabled) {
            vgaterm_print(VGATERM_COLOR_RESET);
        }
    }
    
    term->cursor_x = 7;
    term->cursor_y = 4 + count + 1;
    vgaterm_print(VGATERM_COLOR_PROMPT);
    vgaterm_print("Select: ");
    vgaterm_print(VGATERM_COLOR_RESET);
    
    while (true) {
        char key = vgaterm_wait_key();
        
        if (key >= '1' && key <= '9') {
            int choice = key - '1';
            if (choice < count && items[choice].enabled && items[choice].callback) {
                items[choice].callback();
                return;
            }
        }
    }
}

// ===========================================
// TABLES
// ===========================================

void vgaterm_table(table_t* table) {
    if (!table || !table->headers || !table->data) return;
    
    // Calculate column widths if not provided
    int* widths = table->column_widths;
    bool free_widths = false;
    
    if (!widths) {
        widths = (int*)alloc(table->column_count * sizeof(int));
        free_widths = true;
        
        // Initialize with header lengths
        for (int col = 0; col < table->column_count; col++) {
            widths[col] = strlen(table->headers[col]);
        }
        
        // Check data rows
        for (int row = 0; row < table->row_count; row++) {
            for (int col = 0; col < table->column_count; col++) {
                int len = strlen(table->data[row][col]);
                if (len > widths[col]) {
                    widths[col] = len;
                }
            }
        }
        
        // Add padding
        for (int col = 0; col < table->column_count; col++) {
            widths[col] += 2;
        }
    }
    
    // Draw top border
    vgaterm_print("┌");
    for (int col = 0; col < table->column_count; col++) {
        for (int i = 0; i < widths[col]; i++) {
            vgaterm_print("─");
        }
        if (col < table->column_count - 1) {
            vgaterm_print("┬");
        }
    }
    vgaterm_print("┐\n");
    
    // Draw headers
    vgaterm_print("│");
    for (int col = 0; col < table->column_count; col++) {
        vgaterm_print(VGATERM_COLOR_INFO);
        vgaterm_print(" ");
        vgaterm_print(table->headers[col]);
        
        // Padding
        int padding = widths[col] - strlen(table->headers[col]) - 1;
        for (int i = 0; i < padding; i++) {
            vgaterm_print(" ");
        }
        
        vgaterm_print(VGATERM_COLOR_RESET);
        vgaterm_print("│");
    }
    vgaterm_print("\n");
    
    // Draw separator
    vgaterm_print("├");
    for (int col = 0; col < table->column_count; col++) {
        for (int i = 0; i < widths[col]; i++) {
            vgaterm_print("─");
        }
        if (col < table->column_count - 1) {
            vgaterm_print("┼");
        }
    }
    vgaterm_print("┤\n");
    
    // Draw data rows
    for (int row = 0; row < table->row_count; row++) {
        vgaterm_print("│");
        for (int col = 0; col < table->column_count; col++) {
            vgaterm_print(" ");
            vgaterm_print(table->data[row][col]);
            
            // Padding
            int padding = widths[col] - strlen(table->data[row][col]) - 1;
            for (int i = 0; i < padding; i++) {
                vgaterm_print(" ");
            }
            
            vgaterm_print("│");
        }
        vgaterm_print("\n");
    }
    
    // Draw bottom border
    vgaterm_print("└");
    for (int col = 0; col < table->column_count; col++) {
        for (int i = 0; i < widths[col]; i++) {
            vgaterm_print("─");
        }
        if (col < table->column_count - 1) {
            vgaterm_print("┴");
        }
    }
    vgaterm_print("┘\n");
    
    if (free_widths) {
        free_mem(widths);
    }
}

// ===========================================
// ANIMATIONS
// ===========================================

animation_t* vgaterm_animation_create(uint16_t x, uint16_t y, 
                                      const char** frames, int count,
                                      uint32_t delay_ms) {
    animation_t* anim = (animation_t*)alloc(sizeof(animation_t));
    if (!anim) return NULL;
    
    anim->x = x;
    anim->y = y;
    anim->frames = frames;
    anim->frame_count = count;
    anim->current_frame = 0;
    anim->delay_ms = delay_ms;
    anim->last_update = time_get_uptime_ms();
    
    return anim;
}

void vgaterm_animation_update(animation_t* anim) {
    if (!anim) return;
    
    uint64_t now = time_get_uptime_ms();
    if (now - anim->last_update >= anim->delay_ms) {
        anim->current_frame = (anim->current_frame + 1) % anim->frame_count;
        anim->last_update = now;
        
        // Render current frame
        terminal_t* term = graphics_get_terminal();
        uint16_t old_x = term->cursor_x;
        uint16_t old_y = term->cursor_y;
        
        term->cursor_x = anim->x;
        term->cursor_y = anim->y;
        
        vgaterm_print(anim->frames[anim->current_frame]);
        
        term->cursor_x = old_x;
        term->cursor_y = old_y;
    }
}

void vgaterm_animation_destroy(animation_t* anim) {
    if (anim) {
        free_mem(anim);
    }
}

// ===========================================
// UTILITY FUNCTIONS
// ===========================================

void vgaterm_print_centered(uint16_t row, const char* text) {
    terminal_t* term = graphics_get_terminal();
    uint16_t text_len = strlen(text);
    uint16_t x = (term->cols - text_len) / 2;
    
    term->cursor_x = x;
    term->cursor_y = row;
    
    vgaterm_print(text);
}

void vgaterm_print_at(uint16_t col, uint16_t row, const char* text) {
    terminal_t* term = graphics_get_terminal();
    term->cursor_x = col;
    term->cursor_y = row;
    vgaterm_print(text);
}

void vgaterm_cursor_save(void) {
    terminal_t* term = graphics_get_terminal();
    saved_cursor_x = term->cursor_x;
    saved_cursor_y = term->cursor_y;
}

void vgaterm_cursor_restore(void) {
    terminal_t* term = graphics_get_terminal();
    term->cursor_x = saved_cursor_x;
    term->cursor_y = saved_cursor_y;
}

void vgaterm_clear_line(uint16_t row) {
    terminal_t* term = graphics_get_terminal();
    uint16_t old_x = term->cursor_x;
    uint16_t old_y = term->cursor_y;
    
    term->cursor_x = 0;
    term->cursor_y = row;
    
    for (uint16_t i = 0; i < term->cols; i++) {
        graphics_write_textr_char(' ');
    }
    
    term->cursor_x = old_x;
    term->cursor_y = old_y;
}

void vgaterm_hline(uint16_t row, uint16_t col, uint16_t length, char ch) {
    terminal_t* term = graphics_get_terminal();
    uint16_t old_x = term->cursor_x;
    uint16_t old_y = term->cursor_y;
    
    term->cursor_x = col;
    term->cursor_y = row;
    
    for (uint16_t i = 0; i < length; i++) {
        graphics_write_textr_char(ch);
    }
    
    term->cursor_x = old_x;
    term->cursor_y = old_y;
}

char key = 0;
bool pressed = false;

void callback(uint8_t scancode, char character, bool down) {
    if (down && character) {
        key = character;
        pressed = true;
    }
}

char vgaterm_wait_key(void) {

    // Set callback
    usb_keyboard_set_callback(callback);

    // Poll until key pressed
    while (!pressed) {
        usb_keyboard_update(); // process repeats and held keys
        // Optionally: call some USB polling function if needed
    }

    // Reset callback to NULL or original handler if necessary
    usb_keyboard_set_callback(NULL);

    return key;
}

void vgaterm_beep(void) {
    // TODO: Integrate with PC speaker or audio system
    // For now, just print bell character
    graphics_write_textr_char('\a');
}

// ===========================================
// EXAMPLE USAGE
// ===========================================

/*
// Example 1: Color printing
void example_colors(void) {
    vgaterm_print("/cr255g0b0/ERROR: /cr255g255b255/Connection lost\n");
    vgaterm_print_success("File saved successfully!\n");
    vgaterm_print_warning("Low disk space\n");
    vgaterm_print_info("Server started on port 8080\n");
}

// Example 2: Prompts
void example_prompts(void) {
    if (vgaterm_ask_yn("Delete file?", false)) {
        vgaterm_print_success("Deleted!\n");
    }
    
    const char* options[] = {"Save", "Discard", "Cancel"};
    int choice = vgaterm_ask_choice("Unsaved changes:", options, 3, 0);
    
    char password[64];
    vgaterm_ask_password("Enter password", password, sizeof(password));
}

// Example 3: Loading bars
void example_loadbars(void) {
    // Progress bar
    vgaterm_loadbar_t* bar = vgaterm_loadbar_create(5, 30, true, 
                                                    LOADBAR_STYLE_PROGRESS);
    
    for (int i = 0; i <= 100; i += 10) {
        vgaterm_loadbar_set_progress(bar, i);
        vgaterm_loadbar_render(bar);
        // sleep(100);  // Wait 100ms
    }
    
    vgaterm_loadbar_destroy(bar);
    
    // Service status
    vgaterm_loadbar_t* service = vgaterm_loadbar_create(6, 30, false,
                                                         LOADBAR_STYLE_SERVICE);
    
    vgaterm_loadbar_set_service_state(service, SERVICE_STARTING, 
                                       "Starting network...");
    vgaterm_loadbar_render(service);
    // sleep(1000);
    
    vgaterm_loadbar_set_service_state(service, SERVICE_OK, 
                                       "Network ready");
    vgaterm_loadbar_render(service);
    
    vgaterm_loadbar_destroy(service);
}

// Example 4: Tables
void example_table(void) {
    const char* headers[] = {"Name", "Size", "Date"};
    const char* row1[] = {"file.txt", "1.2KB", "2026-03-26"};
    const char* row2[] = {"data.bin", "512B", "2026-03-25"};
    const char* row3[] = {"image.png", "24KB", "2026-03-20"};
    const char** rows[] = {row1, row2, row3};
    
    table_t table = {headers, rows, 3, 3, NULL};
    vgaterm_table(&table);
}

// Example 5: Status indicators
void example_status(void) {
    vgaterm_status(STATUS_INFO, "System initializing...");
    vgaterm_status(STATUS_OK, "Filesystem mounted");
    vgaterm_status(STATUS_WARN, "Low memory");
    vgaterm_status(STATUS_ERROR, "Network unreachable");
}

// Example 6: Box drawing
void example_boxes(void) {
    vgaterm_draw_box(10, 5, 60, 15, BOX_SINGLE, "Settings");
    vgaterm_draw_box(15, 10, 50, 8, BOX_DOUBLE, "Warning");
    vgaterm_draw_box(20, 15, 40, 5, BOX_ROUNDED, NULL);
}

// Example 7: Menu
void do_new(void) { vgaterm_print_success("Creating new file...\n"); }
void do_open(void) { vgaterm_print_info("Opening file...\n"); }
void do_exit(void) { vgaterm_print_error("Exiting...\n"); }

void example_menu(void) {
    menu_item_t items[] = {
        {"New File", do_new, true},
        {"Open File", do_open, true},
        {"Save File", NULL, false},  // Disabled
        {"Exit", do_exit, true}
    };
    
    vgaterm_menu("File Menu", items, 4);
}
*/