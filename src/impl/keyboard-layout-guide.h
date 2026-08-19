/*
 * KEYBOARD LAYOUT FILE FORMAT (.kbr)
 * 
 * This file documents the .kbr (keyboard) file format used by MinimalOS
 * to define custom keyboard layouts.
 */

// ===========================================
// FILE STRUCTURE
// ===========================================

/*
    Offset   Size    Field
    ------   ----    -----
    0x0000   4       Magic ("KBR1")
    0x0004   2       Version (1)
    0x0006   2       Flags
    0x0008   64      Layout Name
    0x0048   16      Language Code
    0x0058   32      Author
    0x0078   32      Reserved
    0x0098   256     Normal Layer
    0x0198   256     Shift Layer
    0x0298   256     AltGr Layer
    0x0398   256     Shift+AltGr Layer
    0x0498   256     Ctrl Layer
    
    Total: 1432 bytes (0x598)
*/

// ===========================================
// HEADER FIELDS
// ===========================================

/*
Magic (4 bytes): "KBR1" - File format identifier

Version (2 bytes): Format version number
    Current: 1

Flags (2 bytes): Layout properties
    Bit 0: Right-to-left layout (Arabic, Hebrew)
    Bit 1: Has dead keys (accents, diacritics)
    Bit 2: Uses AltGr layer
    Bit 3: Custom/non-standard layout
    Bits 4-15: Reserved

Layout Name (64 bytes): Human-readable name
    Examples: "US QWERTY", "German QWERTZ", "Dvorak"
    
Language Code (16 bytes): ISO language code
    Examples: "en-US", "de-DE", "fr-FR"
    
Author (32 bytes): Layout creator
    Example: "MinimalOS Team"
    
Reserved (32 bytes): For future use, must be zero
*/

// ===========================================
// LAYER DEFINITIONS
// ===========================================

/*
Each layer is a 256-byte array where:
    - Index = USB HID scancode (0x00-0xFF)
    - Value = ASCII character to produce

Normal Layer:
    No modifiers pressed
    Letters are lowercase
    Numbers and symbols in normal positions

Shift Layer:
    Shift key pressed
    Letters are uppercase
    Symbols accessed via Shift+number

AltGr Layer:
    Right Alt (AltGr) pressed
    Used for international characters
    Euro (€), accents (é, ñ, ü), etc.

Shift+AltGr Layer:
    Both Shift and AltGr pressed
    Less common combinations

Ctrl Layer:
    Control key pressed
    Produces control characters (Ctrl+C = 0x03)
*/

// ===========================================
// EXAMPLE: Creating a .kbr file
// ===========================================

/*
// Python script to generate a .kbr file

import struct

def create_kbr_file(filename):
    # Header
    magic = b'KBR1'
    version = 1
    flags = 0
    name = b'Custom Layout\x00' + b'\x00' * (64 - 14)
    language = b'en-US\x00' + b'\x00' * (16 - 6)
    author = b'Your Name\x00' + b'\x00' * (32 - 10)
    reserved = b'\x00' * 32
    
    # Normal layer (all zeros initially)
    normal = bytearray(256)
    normal[0x04] = ord('a')  # USB_KEY_A
    normal[0x05] = ord('b')  # USB_KEY_B
    # ... etc for all keys
    
    # Shift layer
    shift = bytearray(256)
    shift[0x04] = ord('A')  # USB_KEY_A + Shift
    shift[0x05] = ord('B')  # USB_KEY_B + Shift
    # ... etc
    
    # AltGr, Shift+AltGr, Ctrl layers
    altgr = bytearray(256)
    shift_altgr = bytearray(256)
    ctrl = bytearray(256)
    
    # Write file
    with open(filename, 'wb') as f:
        f.write(magic)
        f.write(struct.pack('<H', version))
        f.write(struct.pack('<H', flags))
        f.write(name)
        f.write(language)
        f.write(author)
        f.write(reserved)
        f.write(normal)
        f.write(shift)
        f.write(altgr)
        f.write(shift_altgr)
        f.write(ctrl)

create_kbr_file('custom.kbr')
*/

// ===========================================
// EXAMPLE: Loading a .kbr file in MinimalOS
// ===========================================

/*
// C code example

#include "usb_keyboard.h"

void load_custom_layout(void) {
    // .kbr file loaded into memory
    extern uint8_t _binary_custom_kbr_start[];
    extern uint8_t _binary_custom_kbr_end[];
    
    uint32_t size = _binary_custom_kbr_end - _binary_custom_kbr_start;
    
    // Validate and load
    if (usb_keyboard_validate_kbr(_binary_custom_kbr_start, size)) {
        if (usb_keyboard_load_kbr(_binary_custom_kbr_start, size)) {
            serial_write_str("Keyboard layout loaded successfully!\n");
        }
    } else {
        serial_write_str("Invalid .kbr file!\n");
    }
}
*/

// ===========================================
// BUILT-IN LAYOUT EXAMPLE: German QWERTZ
// ===========================================
/*
const keyboard_layout_t usb_layout_de_qwertz = {
    .header = {
        .magic = {'K', 'B', 'R', '1'},
        .version = 1,
        .flags = KBR_FLAG_ALTGR,  // Uses AltGr
        .name = "German QWERTZ",
        .language = "de-DE",
        .author = "MinimalOS",
    },
    
    .normal = {
        // German layout differences
        [USB_KEY_Y] = 'z',  // Y and Z swapped
        [USB_KEY_Z] = 'y',
        [USB_KEY_MINUS] = 0xDF,  // ß (sharp s)
        // ... etc
    },
    
    .shift = {
        [USB_KEY_Y] = 'Z',
        [USB_KEY_Z] = 'Y',
        [USB_KEY_MINUS] = '?',
        // ... etc
    },
    
    .altgr = {
        // AltGr layer for € and special chars
        [USB_KEY_E] = 0x80,  // € (Euro sign)
        [USB_KEY_2] = 0xB2,  // ² (superscript 2)
        [USB_KEY_3] = 0xB3,  // ³ (superscript 3)
        // ... etc
    },
};
*/
// ===========================================
// USAGE IN YOUR OS
// ===========================================

/*
void kernel_main(void) {
    // Initialize USB keyboard driver
    usb_keyboard_init();
    
    // Load built-in US layout (default)
    usb_keyboard_load_layout(&usb_layout_us_qwerty);
    
    // Or load from .kbr file
    // usb_keyboard_load_kbr(kbr_data, kbr_size);
    
    // Register callback for key events
    usb_keyboard_set_callback(on_key_event);
    
    // In USB interrupt handler:
    usb_hid_keyboard_report_t report;
    // ... read report from USB device ...
    usb_keyboard_process_report(&report);
}

void on_key_event(uint8_t scancode, char character, bool pressed) {
    if (pressed) {
        if (character) {
            serial_write_char(character);
        } else {
            // Non-printable key
            serial_write_str("[");
            serial_write_str(usb_keyboard_key_name(scancode));
            serial_write_str("]");
        }
    }
}
*/

// ===========================================
// COMMON LAYOUTS
// ===========================================

/*
Available built-in layouts:

1. usb_layout_us_qwerty    - US QWERTY (default)
2. usb_layout_uk_qwerty    - UK QWERTY (£ symbol, different layout)
3. usb_layout_de_qwertz    - German QWERTZ (Y/Z swapped, umlauts)
4. usb_layout_fr_azerty    - French AZERTY (A/Q swapped)
5. usb_layout_dvorak       - Dvorak Simplified Keyboard
6. usb_layout_colemak      - Colemak (ergonomic alternative)

To switch layouts at runtime:
    usb_keyboard_load_layout(&usb_layout_dvorak);
*/

// ===========================================
// SPECIAL CHARACTERS & DEAD KEYS
// ===========================================

/*
Dead keys (accents) require special handling:
    - First keypress: dead key (no character output)
    - Second keypress: combines with dead key
    
Example: Typing é on French keyboard:
    1. Press ´ (dead key) - no output
    2. Press e - outputs é

Implementation uses KBR_FLAG_DEADKEYS and special logic
in the keyboard driver.
*/