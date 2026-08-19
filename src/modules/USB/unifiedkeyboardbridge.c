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