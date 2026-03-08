#ifndef PS2_USB_BRIDGE_H
#define PS2_USB_BRIDGE_H

/*
 * PS/2 TO USB KEYBOARD BRIDGE
 * 
 * This bridges PS/2 keyboard events to USB HID keyboard reports,
 * allowing the USB keyboard driver to work with PS/2 keyboards.
 * 
 * This gives you IMMEDIATE keyboard support!
 */

// Initialize the PS/2-USB bridge
void ps2_usb_bridge_init(void);

#endif // PS2_USB_BRIDGE_H