#include "keyboard.h"
#include "keyboard/usbkeyboard.h"
#include "keyboard/swedishKeyboard.h"
#include "keyboard/usKeyboard.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

// ===========================================
// PS/2 SCANCODE TO USB HID MAPPING
// ===========================================

// PS/2 Set 1 scancodes to USB HID usage IDs
static const uint8_t ps2_to_usb_map[128] = {
    [0x00] = 0,  // Invalid
    [0x01] = USB_KEY_ESCAPE,
    [0x02] = USB_KEY_1,
    [0x03] = USB_KEY_2,
    [0x04] = USB_KEY_3,
    [0x05] = USB_KEY_4,
    [0x06] = USB_KEY_5,
    [0x07] = USB_KEY_6,
    [0x08] = USB_KEY_7,
    [0x09] = USB_KEY_8,
    [0x0A] = USB_KEY_9,
    [0x0B] = USB_KEY_0,
    [0x0C] = USB_KEY_MINUS,
    [0x0D] = USB_KEY_EQUAL,
    [0x0E] = USB_KEY_BACKSPACE,
    [0x0F] = USB_KEY_TAB,
    [0x10] = USB_KEY_Q,
    [0x11] = USB_KEY_W,
    [0x12] = USB_KEY_E,
    [0x13] = USB_KEY_R,
    [0x14] = USB_KEY_T,
    [0x15] = USB_KEY_Y,
    [0x16] = USB_KEY_U,
    [0x17] = USB_KEY_I,
    [0x18] = USB_KEY_O,
    [0x19] = USB_KEY_P,
    [0x1A] = USB_KEY_LEFTBRACE,
    [0x1B] = USB_KEY_RIGHTBRACE,
    [0x1C] = USB_KEY_ENTER,
    [0x1D] = USB_KEY_LEFTCTRL,
    [0x1E] = USB_KEY_A,
    [0x1F] = USB_KEY_S,
    [0x20] = USB_KEY_D,
    [0x21] = USB_KEY_F,
    [0x22] = USB_KEY_G,
    [0x23] = USB_KEY_H,
    [0x24] = USB_KEY_J,
    [0x25] = USB_KEY_K,
    [0x26] = USB_KEY_L,
    [0x27] = USB_KEY_SEMICOLON,
    [0x28] = USB_KEY_APOSTROPHE,
    [0x29] = USB_KEY_GRAVE,
    [0x2A] = USB_KEY_LEFTSHIFT,
    [0x2B] = USB_KEY_BACKSLASH,
    [0x2C] = USB_KEY_Z,
    [0x2D] = USB_KEY_X,
    [0x2E] = USB_KEY_C,
    [0x2F] = USB_KEY_V,
    [0x30] = USB_KEY_B,
    [0x31] = USB_KEY_N,
    [0x32] = USB_KEY_M,
    [0x33] = USB_KEY_COMMA,
    [0x34] = USB_KEY_DOT,
    [0x35] = USB_KEY_SLASH,
    [0x36] = USB_KEY_RIGHTSHIFT,
    [0x37] = USB_KEY_KPASTERISK,
    [0x38] = USB_KEY_LEFTALT,
    [0x39] = USB_KEY_SPACE,
    [0x3A] = USB_KEY_CAPSLOCK,
    [0x3B] = USB_KEY_F1,
    [0x3C] = USB_KEY_F2,
    [0x3D] = USB_KEY_F3,
    [0x3E] = USB_KEY_F4,
    [0x3F] = USB_KEY_F5,
    [0x40] = USB_KEY_F6,
    [0x41] = USB_KEY_F7,
    [0x42] = USB_KEY_F8,
    [0x43] = USB_KEY_F9,
    [0x44] = USB_KEY_F10,
    [0x45] = USB_KEY_NUMLOCK,
    [0x46] = USB_KEY_SCROLLLOCK,
    [0x47] = USB_KEY_KP7,
    [0x48] = USB_KEY_KP8,
    [0x49] = USB_KEY_KP9,
    [0x4A] = USB_KEY_KPMINUS,
    [0x4B] = USB_KEY_KP4,
    [0x4C] = USB_KEY_KP5,
    [0x4D] = USB_KEY_KP6,
    [0x4E] = USB_KEY_KPPLUS,
    [0x4F] = USB_KEY_KP1,
    [0x50] = USB_KEY_KP2,
    [0x51] = USB_KEY_KP3,
    [0x52] = USB_KEY_KP0,
    [0x53] = USB_KEY_KPDOT,
    [0x57] = USB_KEY_F11,
    [0x58] = USB_KEY_F12,
};

// Extended PS/2 scancodes (0xE0 prefix)
static const uint8_t ps2_extended_to_usb_map[128] = {
    [0x1C] = USB_KEY_KPENTER,
    [0x1D] = USB_KEY_RIGHTCTRL,
    [0x35] = USB_KEY_KPSLASH,
    [0x38] = USB_KEY_RIGHTALT,
    [0x47] = USB_KEY_HOME,
    [0x48] = USB_KEY_UP,
    [0x49] = USB_KEY_PAGEUP,
    [0x4B] = USB_KEY_LEFT,
    [0x4D] = USB_KEY_RIGHT,
    [0x4F] = USB_KEY_END,
    [0x50] = USB_KEY_DOWN,
    [0x51] = USB_KEY_PAGEDOWN,
    [0x52] = USB_KEY_INSERT,
    [0x53] = USB_KEY_DELETE,
};

// ===========================================
// PS/2 KEYBOARD STATE
// ===========================================

static usb_hid_keyboard_report_t current_report = {0};
static usb_hid_keyboard_report_t previous_report = {0};

// ===========================================
// PS/2 EVENT HANDLER (FIXED - NO GARBAGE!)
// ===========================================

void ps2_keyboard_event_handler(struct KeyboardEvent event) {
    // Extract codes
    bool is_extended = (event.code & 0xE000) != 0;
    uint8_t ps2_code = event.code & 0x7F;
    bool is_make = (event.type == KEYBOARD_EVENT_TYPE_MAKE);
    
    // Debug: log raw PS/2 event
    serial_write_str("PS/2: ");
    serial_write_str(is_make ? "MAKE" : "BREAK");
    serial_write_str(" code=0x");
    serial_write_hex(event.code);
    serial_write_str(" ps2=0x");
    serial_write_hex(ps2_code);
    if (is_extended) serial_write_str(" [EXT]");
    
    // Convert PS/2 to USB scancode
    uint8_t usb_code;
    if (is_extended) {
        usb_code = ps2_extended_to_usb_map[ps2_code];
    } else {
        usb_code = ps2_to_usb_map[ps2_code];
    }
    
    if (usb_code == 0) {
        // Unknown/unmapped key - ignore silently
        serial_write_str(" -> UNMAPPED, ignoring\n");
        return;
    }
    
    serial_write_str(" -> USB 0x");
    serial_write_hex(usb_code);
    serial_write_str("\n");
    
    // Handle modifier keys (don't add to keys array)
    if (usb_code >= USB_KEY_LEFTCTRL && usb_code <= USB_KEY_RIGHTMETA) {
        uint8_t mod_bit = usb_code - USB_KEY_LEFTCTRL;
        if (is_make) {
            current_report.modifiers |= (1 << mod_bit);
        } else {
            current_report.modifiers &= ~(1 << mod_bit);
        }
    } else {
        // Regular key
        if (is_make) {
            // Check if already in array (avoid duplicates)
            bool already_present = false;
            for (int i = 0; i < 6; i++) {
                if (current_report.keys[i] == usb_code) {
                    already_present = true;
                    break;
                }
            }
            
            if (!already_present) {
                // Add to key array
                for (int i = 0; i < 6; i++) {
                    if (current_report.keys[i] == 0) {
                        current_report.keys[i] = usb_code;
                        break;
                    }
                }
            }
        } else {
            // Remove from key array
            for (int i = 0; i < 6; i++) {
                if (current_report.keys[i] == usb_code) {
                    // Shift remaining keys down
                    for (int j = i; j < 5; j++) {
                        current_report.keys[j] = current_report.keys[j + 1];
                    }
                    current_report.keys[5] = 0;
                    break;
                }
            }
        }
    }
    
    // Only send report if it changed
    bool changed = false;
    if (current_report.modifiers != previous_report.modifiers) {
        changed = true;
    }
    for (int i = 0; i < 6; i++) {
        if (current_report.keys[i] != previous_report.keys[i]) {
            changed = true;
            break;
        }
    }
    
    if (changed) {
        serial_write_str("PS/2: Sending HID report\n");
        usb_keyboard_process_report(&current_report);
        previous_report = current_report;
    }
}

// ===========================================
// INITIALIZATION
// ===========================================

void ps2_usb_bridge_init(void) {
    serial_write_str("PS/2-USB Bridge: Initializing...\n");
    
    // Clear reports
    for (int i = 0; i < 6; i++) {
        current_report.keys[i] = 0;
        previous_report.keys[i] = 0;
    }
    current_report.modifiers = 0;
    current_report.reserved = 0;
    previous_report.modifiers = 0;
    previous_report.reserved = 0;
    
    // Initialize USB keyboard driver
    usb_keyboard_init();
    
    // Load Swedish layout (change to usb_layout_us_qwerty if you want US)
    usb_keyboard_load_layout(&usb_layout_se_qwerty);
    
    // Initialize PS/2 keyboard and register our handler
    keyboard_init();
    keyboard_set_handler(ps2_keyboard_event_handler);
    
    serial_write_str("PS/2-USB Bridge: Ready! Keyboard input enabled.\n");
}