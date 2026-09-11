#ifndef USER_USB_H
#define USER_USB_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

static inline uint64_t mos_usb_init(void) {
    return mos_usb(SYS_USB_INIT, 0, 0);
}

static inline uint64_t mos_usb_poll(void) {
    return mos_usb(SYS_USB_POLL, 0, 0);
}

static inline uint64_t mos_usb_has_keyboard(void) {
    return mos_usb(SYS_USB_HAS_KEYBOARD, 0, 0);
}

static inline char mos_usb_key_to_ascii(uint8_t scancode, uint8_t modifiers) {
    return (char)mos_usb(SYS_USB_KEY_TO_ASCII, scancode, modifiers);
}

static inline uint64_t mos_usb_key_is_pressed(uint8_t scancode) {
    return mos_usb(SYS_USB_KEY_IS_PRESSED, scancode, 0);
}

static inline uint64_t mos_usb_keyboard_info(
    syscall_usb_keyboard_info_t* info) {
    return mos_usb(SYS_USB_KEYBOARD_INFO, (uint64_t)info, 0);
}

#endif