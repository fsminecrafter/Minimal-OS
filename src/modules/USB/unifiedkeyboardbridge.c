#include "keyboard/usbkeyboard.h"
#include "usb/usb_stack.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * UNIFIED KEYBOARD BRIDGE
 * 
 * This bridges both USB and PS/2 keyboards to the USB keyboard driver,
 * giving you a single interface regardless of keyboard type.
 */

// ===========================================
// INITIALIZATION
// ===========================================

extern const keyboard_layout_t usb_layout_se_qwerty;

void keyboard_bridge_init(void) {
    serial_write_str("Keyboard Bridge: Initializing...\n");
    
    // Initialize USB keyboard driver
    usb_keyboard_init();
    
    // Load Swedish keyboard layout (change to usb_layout_us_qwerty if you want US)
    usb_keyboard_load_layout(&usb_layout_se_qwerty);
    
    // Check if we have a USB keyboard
    usb_device_t* usb_kbd = usb_get_keyboard();
    
    if (usb_kbd && usb_is_configured(usb_kbd)) {
        serial_write_str("Keyboard Bridge: Using USB keyboard\n");
        serial_write_str("  Vendor:  0x");
        serial_write_hex(usb_kbd->vendor_id);
        serial_write_str("\n  Product: 0x");
        serial_write_hex(usb_kbd->product_id);
        serial_write_str("\n  Speed:   ");
        serial_write_str(usb_get_speed_string(usb_kbd));
        serial_write_str("\n");
        
        // USB keyboard is already initialized by USB stack
        // Polling happens in usb_poll() which calls usb_keyboard_process_report()
    } else {
        serial_write_str("Keyboard Bridge: No USB keyboard, checking PS/2...\n");
        
        // Try PS/2 fallback
        #ifdef HAVE_PS2_KEYBOARD
        extern void ps2_usb_bridge_init(void);
        ps2_usb_bridge_init();
        #else
        serial_write_str("Keyboard Bridge: PS/2 support not compiled in!\n");
        serial_write_str("Keyboard Bridge: No keyboard available!\n");
        #endif
    }
    
    serial_write_str("Keyboard Bridge: Ready!\n");
}

// Note: This is just the initialization bridge.
// The actual keyboard event handling is done by:
// - USB: usb_poll() -> usb_keyboard_process_report() -> callback
// - PS/2: ps2_keyboard_event_handler() -> usb_keyboard_process_report() -> callback
// ===========================================
// PS/2 -> HID BRIDGE
// ===========================================
//
// The PS/2 controller (set up by ps2_controller_init(), scan code
// translation on) delivers set 1 scan codes on IRQ1. They are turned into
// the same 8-byte HID reports a USB keyboard sends and handed to
// usb_keyboard_process_report(), so layouts, key repeat and the input
// event queue behave identically for both kinds of keyboard.

#include "x86_64/port.h"

extern void idt_set_handler_keyboard(void (*handler)());

static const uint8_t g_set1_to_hid[0x59] = {
    [0x01] = 0x29, [0x02] = 0x1E, [0x03] = 0x1F, [0x04] = 0x20, [0x05] = 0x21,
    [0x06] = 0x22, [0x07] = 0x23, [0x08] = 0x24, [0x09] = 0x25, [0x0A] = 0x26,
    [0x0B] = 0x27, [0x0C] = 0x2D, [0x0D] = 0x2E, [0x0E] = 0x2A, [0x0F] = 0x2B,
    [0x10] = 0x14, [0x11] = 0x1A, [0x12] = 0x08, [0x13] = 0x15, [0x14] = 0x17,
    [0x15] = 0x1C, [0x16] = 0x18, [0x17] = 0x0C, [0x18] = 0x12, [0x19] = 0x13,
    [0x1A] = 0x2F, [0x1B] = 0x30, [0x1C] = 0x28,
    [0x1E] = 0x04, [0x1F] = 0x16, [0x20] = 0x07, [0x21] = 0x09, [0x22] = 0x0A,
    [0x23] = 0x0B, [0x24] = 0x0D, [0x25] = 0x0E, [0x26] = 0x0F, [0x27] = 0x33,
    [0x28] = 0x34, [0x29] = 0x35, [0x2B] = 0x31,
    [0x2C] = 0x1D, [0x2D] = 0x1B, [0x2E] = 0x06, [0x2F] = 0x19, [0x30] = 0x05,
    [0x31] = 0x11, [0x32] = 0x10, [0x33] = 0x36, [0x34] = 0x37, [0x35] = 0x38,
    [0x37] = 0x55, [0x39] = 0x2C, [0x3A] = 0x39,
    [0x3B] = 0x3A, [0x3C] = 0x3B, [0x3D] = 0x3C, [0x3E] = 0x3D, [0x3F] = 0x3E,
    [0x40] = 0x3F, [0x41] = 0x40, [0x42] = 0x41, [0x43] = 0x42, [0x44] = 0x43,
    [0x45] = 0x53, [0x46] = 0x47,
    [0x47] = 0x5F, [0x48] = 0x60, [0x49] = 0x61, [0x4A] = 0x56, [0x4B] = 0x5C,
    [0x4C] = 0x5D, [0x4D] = 0x5E, [0x4E] = 0x57, [0x4F] = 0x59, [0x50] = 0x5A,
    [0x51] = 0x5B, [0x52] = 0x62, [0x53] = 0x63,
    [0x56] = 0x64, [0x57] = 0x44, [0x58] = 0x45,
};

static uint8_t ps2_extended_to_hid(uint8_t code) {
    switch (code) {
        case 0x1C: return 0x58;   // keypad Enter
        case 0x35: return 0x54;   // keypad /
        case 0x47: return 0x4A;   // Home
        case 0x48: return 0x52;   // Up
        case 0x49: return 0x4B;   // Page Up
        case 0x4B: return 0x50;   // Left
        case 0x4D: return 0x4F;   // Right
        case 0x4F: return 0x4D;   // End
        case 0x50: return 0x51;   // Down
        case 0x51: return 0x4E;   // Page Down
        case 0x52: return 0x49;   // Insert
        case 0x53: return 0x4C;   // Delete
        case 0x5D: return 0x65;   // Menu
        default: return 0;
    }
}

static usb_hid_keyboard_report_t g_ps2_report;
static bool g_ps2_extended;
static uint8_t g_ps2_skip;          // bytes left of a Pause sequence

static void ps2_report_key(uint8_t usage, bool down) {
    if (down) {
        for (int i = 0; i < 6; i++) if (g_ps2_report.keys[i] == usage) return;
        for (int i = 0; i < 6; i++) {
            if (g_ps2_report.keys[i] == 0) { g_ps2_report.keys[i] = usage; return; }
        }
    } else {
        for (int i = 0; i < 6; i++) {
            if (g_ps2_report.keys[i] == usage) {
                for (int j = i; j < 5; j++) g_ps2_report.keys[j] = g_ps2_report.keys[j + 1];
                g_ps2_report.keys[5] = 0;
                return;
            }
        }
    }
}

static void ps2_keyboard_irq(void) {
    if (!(port_inb(0x64) & 0x01)) return;
    uint8_t byte = port_inb(0x60);

    if (g_ps2_skip) { g_ps2_skip--; return; }
    if (byte == 0xE1) { g_ps2_skip = 5; return; }    // Pause: ignore
    if (byte == 0xE0) { g_ps2_extended = true; return; }
    if (byte == 0xFA || byte == 0xFE || byte == 0x00 || byte == 0xFF) return;

    bool extended = g_ps2_extended;
    g_ps2_extended = false;
    bool down = (byte & 0x80) == 0;
    uint8_t code = byte & 0x7F;

    uint8_t mod = 0;
    if (!extended) {
        if (code == 0x1D) mod = USB_MOD_LCTRL;
        else if (code == 0x2A) mod = USB_MOD_LSHIFT;
        else if (code == 0x36) mod = USB_MOD_RSHIFT;
        else if (code == 0x38) mod = USB_MOD_LALT;
    } else {
        if (code == 0x2A || code == 0x36) return;      // fake shifts
        if (code == 0x1D) mod = USB_MOD_RCTRL;
        else if (code == 0x38) mod = USB_MOD_RALT;
        else if (code == 0x5B) mod = USB_MOD_LMETA;
        else if (code == 0x5C) mod = USB_MOD_RMETA;
    }

    if (mod) {
        if (down) g_ps2_report.modifiers |= mod;
        else g_ps2_report.modifiers &= (uint8_t)~mod;
    } else {
        uint8_t usage = extended ? ps2_extended_to_hid(code)
                                 : (code < sizeof(g_set1_to_hid) ? g_set1_to_hid[code] : 0);
        if (!usage) return;
        ps2_report_key(usage, down);
    }
    usb_keyboard_process_report(&g_ps2_report);
}

void ps2_keyboard_bridge_init(void) {
    idt_set_handler_keyboard(ps2_keyboard_irq);
    serial_write_str("Keyboard Bridge: PS/2 keyboard events go to the keyboard driver\n");
}
