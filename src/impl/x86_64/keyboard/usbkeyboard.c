#include "keyboard/usbkeyboard.h"
#include "serial.h"
#include "string.h"
#include "time.h"
#include <stddef.h>

// ===========================================
// GLOBAL STATE
// ===========================================

static usb_keyboard_state_t g_keyboard_state = {0};
static usb_keyboard_callback_t g_user_callback = NULL;

// Key name lookup table
static const char* g_key_names[256] = {
    [USB_KEY_NONE] = "None",
    [USB_KEY_A] = "A", [USB_KEY_B] = "B", [USB_KEY_C] = "C", [USB_KEY_D] = "D",
    [USB_KEY_E] = "E", [USB_KEY_F] = "F", [USB_KEY_G] = "G", [USB_KEY_H] = "H",
    [USB_KEY_I] = "I", [USB_KEY_J] = "J", [USB_KEY_K] = "K", [USB_KEY_L] = "L",
    [USB_KEY_M] = "M", [USB_KEY_N] = "N", [USB_KEY_O] = "O", [USB_KEY_P] = "P",
    [USB_KEY_Q] = "Q", [USB_KEY_R] = "R", [USB_KEY_S] = "S", [USB_KEY_T] = "T",
    [USB_KEY_U] = "U", [USB_KEY_V] = "V", [USB_KEY_W] = "W", [USB_KEY_X] = "X",
    [USB_KEY_Y] = "Y", [USB_KEY_Z] = "Z",
    [USB_KEY_1] = "1", [USB_KEY_2] = "2", [USB_KEY_3] = "3", [USB_KEY_4] = "4",
    [USB_KEY_5] = "5", [USB_KEY_6] = "6", [USB_KEY_7] = "7", [USB_KEY_8] = "8",
    [USB_KEY_9] = "9", [USB_KEY_0] = "0",
    [USB_KEY_ENTER] = "Enter", [USB_KEY_ESCAPE] = "Escape",
    [USB_KEY_BACKSPACE] = "Backspace", [USB_KEY_TAB] = "Tab",
    [USB_KEY_SPACE] = "Space", [USB_KEY_MINUS] = "Minus", [USB_KEY_EQUAL] = "Equal",
    [USB_KEY_LEFTBRACE] = "LeftBrace", [USB_KEY_RIGHTBRACE] = "RightBrace",
    [USB_KEY_BACKSLASH] = "Backslash", [USB_KEY_SEMICOLON] = "Semicolon",
    [USB_KEY_APOSTROPHE] = "Apostrophe", [USB_KEY_GRAVE] = "Grave",
    [USB_KEY_COMMA] = "Comma", [USB_KEY_DOT] = "Dot", [USB_KEY_SLASH] = "Slash",
    [USB_KEY_CAPSLOCK] = "CapsLock",
    [USB_KEY_F1] = "F1", [USB_KEY_F2] = "F2", [USB_KEY_F3] = "F3", [USB_KEY_F4] = "F4",
    [USB_KEY_F5] = "F5", [USB_KEY_F6] = "F6", [USB_KEY_F7] = "F7", [USB_KEY_F8] = "F8",
    [USB_KEY_F9] = "F9", [USB_KEY_F10] = "F10", [USB_KEY_F11] = "F11", [USB_KEY_F12] = "F12",
    [USB_KEY_PRINTSCREEN] = "PrintScreen", [USB_KEY_SCROLLLOCK] = "ScrollLock",
    [USB_KEY_PAUSE] = "Pause", [USB_KEY_INSERT] = "Insert",
    [USB_KEY_HOME] = "Home", [USB_KEY_PAGEUP] = "PageUp",
    [USB_KEY_DELETE] = "Delete", [USB_KEY_END] = "End", [USB_KEY_PAGEDOWN] = "PageDown",
    [USB_KEY_RIGHT] = "Right", [USB_KEY_LEFT] = "Left",
    [USB_KEY_DOWN] = "Down", [USB_KEY_UP] = "Up",
    [USB_KEY_NUMLOCK] = "NumLock",
};

// ===========================================
// FORWARD DECLARATIONS
// ===========================================

static void usb_keyboard_handle_key_press(uint8_t scancode, uint8_t modifiers);
static void usb_keyboard_handle_key_release(uint8_t scancode);

// ===========================================
// INITIALIZATION
// ===========================================

void usb_keyboard_init(void) {
    serial_write_str("USB Keyboard: Initializing...\n");
    
    memset(&g_keyboard_state, 0, sizeof(usb_keyboard_state_t));
    
    g_keyboard_state.caps_lock = false;
    g_keyboard_state.num_lock = true;
    g_keyboard_state.scroll_lock = false;
    
    // SLOWER, MORE REASONABLE repeat settings:
    g_keyboard_state.repeat_delay_ms = 600;   // 600ms before repeat (was 500ms)
    g_keyboard_state.repeat_rate_ms = 80;     // 80ms between repeats (was 33ms)
                                               // = ~12 chars/sec (was 30/sec)
    
    g_keyboard_state.layout = NULL;
    
    serial_write_str("USB Keyboard: Ready (no layout loaded)\n");
}
// ===========================================
// REPORT PROCESSING
// ===========================================

void usb_keyboard_process_report(const usb_hid_keyboard_report_t* report) {
    if (!report) return;

    g_keyboard_state.previous_report = g_keyboard_state.current_report;
    g_keyboard_state.current_report = *report;
    
    uint8_t mods = report->modifiers;

    g_keyboard_state.ctrl  = (mods & (USB_MOD_LCTRL  | USB_MOD_RCTRL))  != 0;
    g_keyboard_state.shift = (mods & (USB_MOD_LSHIFT | USB_MOD_RSHIFT)) != 0;
    g_keyboard_state.alt   = (mods & (USB_MOD_LALT   | USB_MOD_RALT))   != 0;
    g_keyboard_state.meta  = (mods & (USB_MOD_LMETA  | USB_MOD_RMETA))  != 0;

    // Detect new presses
    for (int i = 0; i < 6; i++) {
        uint8_t scancode = report->keys[i];
        if (scancode == 0) continue;

        bool was_pressed = false;

        for (int j = 0; j < 6; j++) {
            if (g_keyboard_state.previous_report.keys[j] == scancode) {
                was_pressed = true;
                break;
            }
        }

        if (!was_pressed) {
            usb_keyboard_handle_key_press(scancode, mods);
        }
    }

    // Detect releases
    for (int i = 0; i < 6; i++) {
        uint8_t old_key = g_keyboard_state.previous_report.keys[i];
        if (old_key == 0) continue;

        bool still_pressed = false;

        for (int j = 0; j < 6; j++) {
            if (report->keys[j] == old_key) {
                still_pressed = true;
                break;
            }
        }

        if (!still_pressed) {
            usb_keyboard_handle_key_release(old_key);
        }
    }

}

static void usb_keyboard_handle_key_press(uint8_t scancode, uint8_t modifiers) {
    // Handle toggle keys
    bool led_update = false;
    
    switch (scancode) {
        case USB_KEY_CAPSLOCK:
            g_keyboard_state.caps_lock = !g_keyboard_state.caps_lock;
            led_update = true;
            break;
            
        case USB_KEY_NUMLOCK:
            g_keyboard_state.num_lock = !g_keyboard_state.num_lock;
            led_update = true;
            break;
            
        case USB_KEY_SCROLLLOCK:
            g_keyboard_state.scroll_lock = !g_keyboard_state.scroll_lock;
            led_update = true;
            break;
    }
    
    if (led_update) {
        usb_keyboard_set_leds(g_keyboard_state.caps_lock,
                             g_keyboard_state.num_lock,
                             g_keyboard_state.scroll_lock);
    }
    
    // Translate to character
    char character = usb_keyboard_translate(scancode, modifiers);
    
    uint64_t now = time_get_uptime_ms();
 
    // Store key info for repeat
    g_keyboard_state.last_key = scancode;
    g_keyboard_state.last_key_time_ms = now;
    g_keyboard_state.last_repeat_number = 0;  // ← RESET repeat counter!
    
    // Don't call callback for modifier-only keys
    if (usb_keyboard_is_modifier(scancode)) {
        return;
    }
    
    // Call user callback for the press
    if (g_user_callback) {
        g_user_callback(scancode, character, true);
    }
}

static void usb_keyboard_handle_key_release(uint8_t scancode) {
    // Clear last key if this was it
    if (g_keyboard_state.last_key == scancode) {
        g_keyboard_state.last_key = 0;
        g_keyboard_state.last_key_time_ms = 0;
        g_keyboard_state.last_repeat_number = 0;
    }
    
    // Don't call callback for modifier releases
    if (usb_keyboard_is_modifier(scancode)) {
        return;
    }
    
    // Call user callback
    if (g_user_callback) {
        g_user_callback(scancode, 0, false);
    }
}

// ===========================================
// CHARACTER TRANSLATION
// ===========================================

char usb_keyboard_translate(uint8_t scancode, uint8_t modifiers) {
    if (!g_keyboard_state.layout) {
        // No layout loaded - try basic ASCII fallback
        if (scancode >= USB_KEY_A && scancode <= USB_KEY_Z) {
            bool shift = (modifiers & (USB_MOD_LSHIFT | USB_MOD_RSHIFT)) != 0;
            if (g_keyboard_state.caps_lock) shift = !shift;
            return shift ? ('A' + (scancode - USB_KEY_A)) : ('a' + (scancode - USB_KEY_A));
        }
        return 0;
    }
    
    if (scancode >= 256) return 0;
    
    bool shift = (modifiers & (USB_MOD_LSHIFT | USB_MOD_RSHIFT)) != 0;
    bool ctrl = (modifiers & (USB_MOD_LCTRL | USB_MOD_RCTRL)) != 0;
    bool altgr = (modifiers & USB_MOD_RALT) != 0;
    
    // Apply caps lock for letters
    if (g_keyboard_state.caps_lock && scancode >= USB_KEY_A && scancode <= USB_KEY_Z) {
        shift = !shift;
    }
    
    // Apply num lock for numpad
    if (g_keyboard_state.num_lock && scancode >= USB_KEY_KP1 && scancode <= USB_KEY_KP0) {
        // Numpad keys act as numbers when num lock is on
        shift = false;
    }
    
    // Select appropriate layer
    char character;
    if (ctrl) {
        character = g_keyboard_state.layout->ctrl[scancode];
    } else if (shift && altgr) {
        character = g_keyboard_state.layout->shift_altgr[scancode];
        if (!character) character = g_keyboard_state.layout->shift[scancode];
    } else if (altgr) {
        character = g_keyboard_state.layout->altgr[scancode];
        if (!character) character = g_keyboard_state.layout->normal[scancode];
    } else if (shift) {
        character = g_keyboard_state.layout->shift[scancode];
    } else {
        character = g_keyboard_state.layout->normal[scancode];
    }
    
    return character;
}

char usb_keyboard_to_ascii(uint8_t scancode) {
    return usb_keyboard_translate(scancode, g_keyboard_state.current_report.modifiers);
}

// ===========================================
// KEY STATE QUERIES
// ===========================================

bool usb_keyboard_is_pressed(uint8_t scancode) {
    for (int i = 0; i < 6; i++) {
        if (g_keyboard_state.current_report.keys[i] == scancode) {
            return true;
        }
    }
    return false;
}

const usb_keyboard_state_t* usb_keyboard_get_state(void) {
    return &g_keyboard_state;
}

// ===========================================
// LAYOUT MANAGEMENT
// ===========================================

bool usb_keyboard_load_layout(const keyboard_layout_t* layout) {
    if (!layout) return false;
    
    g_keyboard_state.layout = (keyboard_layout_t*)layout;
    
    serial_write_str("USB Keyboard: Loaded layout '");
    serial_write_str(layout->header.name);
    serial_write_str("' (");
    serial_write_str(layout->header.language);
    serial_write_str(")\n");
    
    return true;
}

bool usb_keyboard_load_kbr(const uint8_t* kbr_data, uint32_t size) {
    if (!kbr_data || size < sizeof(keyboard_layout_t)) {
        serial_write_str("USB Keyboard: .kbr file too small\n");
        return false;
    }
    
    // Validate
    if (!usb_keyboard_validate_kbr(kbr_data, size)) {
        serial_write_str("USB Keyboard: Invalid .kbr file\n");
        return false;
    }
    
    // Load layout
    keyboard_layout_t* layout = (keyboard_layout_t*)kbr_data;
    return usb_keyboard_load_layout(layout);
}

const keyboard_layout_t* usb_keyboard_get_layout(void) {
    return g_keyboard_state.layout;
}

bool usb_keyboard_validate_kbr(const uint8_t* kbr_data, uint32_t size) {
    if (!kbr_data || size < sizeof(kbr_header_t)) {
        return false;
    }
    
    kbr_header_t* header = (kbr_header_t*)kbr_data;
    
    // Check magic
    if (header->magic[0] != 'K' || 
        header->magic[1] != 'B' || 
        header->magic[2] != 'R' || 
        header->magic[3] != '1') {
        serial_write_str("USB Keyboard: Invalid magic in .kbr\n");
        return false;
    }
    
    // Check version
    if (header->version != KBR_VERSION) {
        serial_write_str("USB Keyboard: Unsupported .kbr version\n");
        return false;
    }
    
    // Check size
    if (size < sizeof(keyboard_layout_t)) {
        serial_write_str("USB Keyboard: .kbr file incomplete\n");
        return false;
    }
    
    return true;
}

const keyboard_layout_t* usb_keyboard_get_builtin_layout(const char* name) {
    // Forward declarations of built-in layouts
    extern const keyboard_layout_t usb_layout_us_qwerty;
    extern const keyboard_layout_t usb_layout_se_qwerty;
    
    if (strcmp(name, "US") == 0 || strcmp(name, "us") == 0) {
        return &usb_layout_us_qwerty;
    }
    if (strcmp(name, "SE") == 0 || strcmp(name, "se") == 0) {
        return &usb_layout_se_qwerty;
    }
    
    return NULL;
}

// ===========================================
// CALLBACK SYSTEM
// ===========================================

void usb_keyboard_set_callback(usb_keyboard_callback_t callback) {
    g_user_callback = callback;
}

// ===========================================
// UTILITY FUNCTIONS
// ===========================================

const char* usb_keyboard_key_name(uint8_t scancode) {
    if (scancode < 256 && g_key_names[scancode]) {
        return g_key_names[scancode];
    }
    return "Unknown";
}

bool usb_keyboard_is_modifier(uint8_t scancode) {
    return scancode >= USB_KEY_LEFTCTRL && scancode <= USB_KEY_RIGHTMETA;
}

bool usb_keyboard_is_printable(uint8_t scancode) {
    // Letters
    if (scancode >= USB_KEY_A && scancode <= USB_KEY_Z) return true;
    // Numbers
    if (scancode >= USB_KEY_1 && scancode <= USB_KEY_0) return true;
    // Symbols and punctuation
    if (scancode >= USB_KEY_MINUS && scancode <= USB_KEY_SLASH) return true;
    // Space
    if (scancode == USB_KEY_SPACE) return true;
    // Numpad
    if (scancode >= USB_KEY_KPSLASH && scancode <= USB_KEY_KPDOT) return true;
    
    return false;
}

void usb_keyboard_set_leds(bool caps, bool num, bool scroll) {
    // Build LED report byte
    uint8_t led_report = 0;
    if (num) led_report |= (1 << 0);    // Num Lock
    if (caps) led_report |= (1 << 1);   // Caps Lock
    if (scroll) led_report |= (1 << 2); // Scroll Lock
    
    // TODO: Send LED report to USB device
    // This requires USB HID output report functionality
    // For now, just log it
    serial_write_str("USB Keyboard: LEDs = ");
    if (caps) serial_write_str("CAPS ");
    if (num) serial_write_str("NUM ");
    if (scroll) serial_write_str("SCROLL ");
    serial_write_str("\n");
}

// ===========================================
// KEY REPEAT HANDLING
// ===========================================

void usb_keyboard_update(void) {
    // No key held? Nothing to do
    if (g_keyboard_state.last_key == 0) {
        return;
    }

    if (!usb_keyboard_is_pressed(g_keyboard_state.last_key)) {
        g_keyboard_state.last_key = 0;
        return;
    }
    
    // Don't repeat modifiers
    if (usb_keyboard_is_modifier(g_keyboard_state.last_key)) {
        return;
    }
    
    uint64_t now = time_get_uptime_ms();
    uint64_t key_held_time = now - g_keyboard_state.last_key_time_ms;
    
    // Not held long enough to start repeating?
    if (key_held_time < g_keyboard_state.repeat_delay_ms) {
        return;
    }
    
    // Time since we started repeating
    uint64_t repeat_time = key_held_time - g_keyboard_state.repeat_delay_ms;
    
    // Calculate which repeat number we should be on
    uint64_t repeat_number = repeat_time / g_keyboard_state.repeat_rate_ms;
    
    // Store the last repeat we sent in the state struct
    // (You'll need to add this field to usb_keyboard_state_t)
    if (repeat_number > g_keyboard_state.last_repeat_number) {
        char character = usb_keyboard_translate(
            g_keyboard_state.last_key,
            g_keyboard_state.current_report.modifiers
        );
        
        if (g_user_callback) {
            g_user_callback(g_keyboard_state.last_key, character, true);
        }
        
        g_keyboard_state.last_repeat_number = repeat_number;
    }
}