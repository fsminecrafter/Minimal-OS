#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

// ===========================================
// USB KEYBOARD SCANCODES (HID Usage IDs)
// ===========================================

// Standard USB HID keyboard scancodes
typedef enum {
    USB_KEY_NONE = 0x00,
    
    // Letters
    USB_KEY_A = 0x04,
    USB_KEY_B = 0x05,
    USB_KEY_C = 0x06,
    USB_KEY_D = 0x07,
    USB_KEY_E = 0x08,
    USB_KEY_F = 0x09,
    USB_KEY_G = 0x0A,
    USB_KEY_H = 0x0B,
    USB_KEY_I = 0x0C,
    USB_KEY_J = 0x0D,
    USB_KEY_K = 0x0E,
    USB_KEY_L = 0x0F,
    USB_KEY_M = 0x10,
    USB_KEY_N = 0x11,
    USB_KEY_O = 0x12,
    USB_KEY_P = 0x13,
    USB_KEY_Q = 0x14,
    USB_KEY_R = 0x15,
    USB_KEY_S = 0x16,
    USB_KEY_T = 0x17,
    USB_KEY_U = 0x18,
    USB_KEY_V = 0x19,
    USB_KEY_W = 0x1A,
    USB_KEY_X = 0x1B,
    USB_KEY_Y = 0x1C,
    USB_KEY_Z = 0x1D,
    
    // Numbers
    USB_KEY_1 = 0x1E,
    USB_KEY_2 = 0x1F,
    USB_KEY_3 = 0x20,
    USB_KEY_4 = 0x21,
    USB_KEY_5 = 0x22,
    USB_KEY_6 = 0x23,
    USB_KEY_7 = 0x24,
    USB_KEY_8 = 0x25,
    USB_KEY_9 = 0x26,
    USB_KEY_0 = 0x27,
    
    // Special keys
    USB_KEY_ENTER = 0x28,
    USB_KEY_ESCAPE = 0x29,
    USB_KEY_BACKSPACE = 0x2A,
    USB_KEY_TAB = 0x2B,
    USB_KEY_SPACE = 0x2C,
    USB_KEY_MINUS = 0x2D,
    USB_KEY_EQUAL = 0x2E,
    USB_KEY_LEFTBRACE = 0x2F,
    USB_KEY_RIGHTBRACE = 0x30,
    USB_KEY_BACKSLASH = 0x31,
    USB_KEY_SEMICOLON = 0x33,
    USB_KEY_APOSTROPHE = 0x34,
    USB_KEY_GRAVE = 0x35,
    USB_KEY_COMMA = 0x36,
    USB_KEY_DOT = 0x37,
    USB_KEY_SLASH = 0x38,
    USB_KEY_CAPSLOCK = 0x39,
    
    // Function keys
    USB_KEY_F1 = 0x3A,
    USB_KEY_F2 = 0x3B,
    USB_KEY_F3 = 0x3C,
    USB_KEY_F4 = 0x3D,
    USB_KEY_F5 = 0x3E,
    USB_KEY_F6 = 0x3F,
    USB_KEY_F7 = 0x40,
    USB_KEY_F8 = 0x41,
    USB_KEY_F9 = 0x42,
    USB_KEY_F10 = 0x43,
    USB_KEY_F11 = 0x44,
    USB_KEY_F12 = 0x45,
    
    // Control keys
    USB_KEY_PRINTSCREEN = 0x46,
    USB_KEY_SCROLLLOCK = 0x47,
    USB_KEY_PAUSE = 0x48,
    USB_KEY_INSERT = 0x49,
    USB_KEY_HOME = 0x4A,
    USB_KEY_PAGEUP = 0x4B,
    USB_KEY_DELETE = 0x4C,
    USB_KEY_END = 0x4D,
    USB_KEY_PAGEDOWN = 0x4E,
    USB_KEY_RIGHT = 0x4F,
    USB_KEY_LEFT = 0x50,
    USB_KEY_DOWN = 0x51,
    USB_KEY_UP = 0x52,
    
    // Numpad
    USB_KEY_NUMLOCK = 0x53,
    USB_KEY_KPSLASH = 0x54,
    USB_KEY_KPASTERISK = 0x55,
    USB_KEY_KPMINUS = 0x56,
    USB_KEY_KPPLUS = 0x57,
    USB_KEY_KPENTER = 0x58,
    USB_KEY_KP1 = 0x59,
    USB_KEY_KP2 = 0x5A,
    USB_KEY_KP3 = 0x5B,
    USB_KEY_KP4 = 0x5C,
    USB_KEY_KP5 = 0x5D,
    USB_KEY_KP6 = 0x5E,
    USB_KEY_KP7 = 0x5F,
    USB_KEY_KP8 = 0x60,
    USB_KEY_KP9 = 0x61,
    USB_KEY_KP0 = 0x62,
    USB_KEY_KPDOT = 0x63,
    
    // Modifiers (appear in modifier byte, not scancode)
    USB_KEY_LEFTCTRL = 0xE0,
    USB_KEY_LEFTSHIFT = 0xE1,
    USB_KEY_LEFTALT = 0xE2,
    USB_KEY_LEFTMETA = 0xE3,
    USB_KEY_RIGHTCTRL = 0xE4,
    USB_KEY_RIGHTSHIFT = 0xE5,
    USB_KEY_RIGHTALT = 0xE6,
    USB_KEY_RIGHTMETA = 0xE7,
} usb_key_t;

// Modifier flags (byte 0 of HID report)
#define USB_MOD_LCTRL   (1 << 0)
#define USB_MOD_LSHIFT  (1 << 1)
#define USB_MOD_LALT    (1 << 2)
#define USB_MOD_LMETA   (1 << 3)
#define USB_MOD_RCTRL   (1 << 4)
#define USB_MOD_RSHIFT  (1 << 5)
#define USB_MOD_RALT    (1 << 6)
#define USB_MOD_RMETA   (1 << 7)

// ===========================================
// KEYBOARD LAYOUT (.kbr file format)
// ===========================================

#define KBR_MAGIC "KBR1"
#define KBR_VERSION 1
#define KBR_MAX_NAME_LEN 64

// Keyboard layout header
typedef struct {
    char magic[4];              // "KBR1"
    uint16_t version;           // Layout format version
    uint16_t flags;             // Layout flags
    char name[KBR_MAX_NAME_LEN]; // Layout name (e.g., "US QWERTY")
    char language[16];          // Language code (e.g., "en-US")
    char author[32];            // Layout author
    uint32_t reserved[8];       // Reserved for future use
} __attribute__((packed)) kbr_header_t;

// Layout flags
#define KBR_FLAG_RTOL        (1 << 0)  // Right-to-left
#define KBR_FLAG_DEADKEYS    (1 << 1)  // Has dead key support
#define KBR_FLAG_ALTGR       (1 << 2)  // Has AltGr layer
#define KBR_FLAG_CUSTOM      (1 << 3)  // Custom/non-standard layout

// Complete keyboard layout definition
typedef struct {
    kbr_header_t header;
    
    // Normal layer (no modifiers)
    char normal[256];
    
    // Shift layer
    char shift[256];
    
    // AltGr layer (right alt)
    char altgr[256];
    
    // Shift + AltGr layer
    char shift_altgr[256];
    
    // Ctrl layer (for control characters)
    char ctrl[256];
} __attribute__((packed)) keyboard_layout_t;

// ===========================================
// USB HID KEYBOARD REPORT
// ===========================================

// Standard USB HID keyboard input report (8 bytes)
typedef struct {
    uint8_t modifiers;          // Modifier byte
    uint8_t reserved;           // Reserved (always 0)
    uint8_t keys[6];            // Up to 6 simultaneous key presses
} __attribute__((packed)) usb_hid_keyboard_report_t;

// ===========================================
// KEYBOARD STATE
// ===========================================

typedef struct {
    // Current HID report
    usb_hid_keyboard_report_t current_report;
    usb_hid_keyboard_report_t previous_report;
    
    // Modifier states
    bool ctrl;
    bool shift;
    bool alt;
    bool meta;
    
    // Lock states
    bool caps_lock;
    bool num_lock;
    bool scroll_lock;
    
    // Active layout
    keyboard_layout_t* layout;
    
    // Key repeat
    uint8_t last_key;
    uint64_t last_key_time_ms;
    uint64_t repeat_delay_ms;
    uint64_t repeat_rate_ms;

    uint64_t last_key_press_time_ms;
    uint64_t next_repeat_time_ms;
    uint64_t last_repeat_number;
    
} usb_keyboard_state_t;

// ===========================================
// KEYBOARD DRIVER FUNCTIONS
// ===========================================

// Initialize USB keyboard driver
void usb_keyboard_init(void);

// Process HID keyboard report (8 bytes from USB)
void usb_keyboard_process_report(const usb_hid_keyboard_report_t* report);

// Get translated character from scancode
char usb_keyboard_translate(uint8_t scancode, uint8_t modifiers);

// Check if key is currently pressed
bool usb_keyboard_is_pressed(uint8_t scancode);

// Get keyboard state
const usb_keyboard_state_t* usb_keyboard_get_state(void);

// ===========================================
// KEYBOARD LAYOUT FUNCTIONS
// ===========================================

// Load keyboard layout from memory
bool usb_keyboard_load_layout(const keyboard_layout_t* layout);

// Load keyboard layout from .kbr file data
bool usb_keyboard_load_kbr(const uint8_t* kbr_data, uint32_t size);

// Get current layout
const keyboard_layout_t* usb_keyboard_get_layout(void);

// Validate .kbr file
bool usb_keyboard_validate_kbr(const uint8_t* kbr_data, uint32_t size);

// ===========================================
// BUILT-IN LAYOUTS
// ===========================================

// Get built-in layout by name
const keyboard_layout_t* usb_keyboard_get_builtin_layout(const char* name);

// Built-in layout: US QWERTY
extern const keyboard_layout_t usb_layout_us_qwerty;

// Built-in layout: UK QWERTY
extern const keyboard_layout_t usb_layout_uk_qwerty;

// Built-in layout: German QWERTZ
extern const keyboard_layout_t usb_layout_de_qwertz;

// Built-in layout: French AZERTY
extern const keyboard_layout_t usb_layout_fr_azerty;

// Built-in layout: Dvorak
extern const keyboard_layout_t usb_layout_dvorak;

// Built-in layout: Colemak
extern const keyboard_layout_t usb_layout_colemak;

// ===========================================
// CALLBACK SYSTEM
// ===========================================

// Callback function type
typedef void (*usb_keyboard_callback_t)(uint8_t scancode, char character, bool pressed);

// Register callback for key events
void usb_keyboard_set_callback(usb_keyboard_callback_t callback);
// Gets the current callback for key events
usb_keyboard_callback_t usb_keyboard_get_callback(void);

// ===========================================
// UTILITY FUNCTIONS
// ===========================================

// Get human-readable key name
const char* usb_keyboard_key_name(uint8_t scancode);

// Check if scancode is a modifier
bool usb_keyboard_is_modifier(uint8_t scancode);

// Check if scancode is a printable key
bool usb_keyboard_is_printable(uint8_t scancode);

// Convert USB scancode to ASCII (using current layout)
char usb_keyboard_to_ascii(uint8_t scancode);

// LED control (for keyboard LEDs)
void usb_keyboard_set_leds(bool caps, bool num, bool scroll);

// Call this in main loop or timer interrupt
// Handles key repeat functionality
void usb_keyboard_update(void);

#endif // USB_KEYBOARD_H