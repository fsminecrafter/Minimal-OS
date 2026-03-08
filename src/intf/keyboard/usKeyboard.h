#ifndef USB_LAYOUT_US_H
#define USB_LAYOUT_US_H

#include "keyboard/usbkeyboard.h"

// US QWERTY Keyboard Layout
// This is embedded as a const structure, but matches .kbr file format

const keyboard_layout_t usb_layout_us_qwerty = {
    .header = {
        .magic = {'K', 'B', 'R', '1'},
        .version = KBR_VERSION,
        .flags = 0,
        .name = "US QWERTY",
        .language = "en-US",
        .author = "MinimalOS",
        .reserved = {0}
    },
    
    // Normal layer (no modifiers)
    .normal = {
        [USB_KEY_NONE] = 0,
        
        // Letters (lowercase)
        [USB_KEY_A] = 'a',
        [USB_KEY_B] = 'b',
        [USB_KEY_C] = 'c',
        [USB_KEY_D] = 'd',
        [USB_KEY_E] = 'e',
        [USB_KEY_F] = 'f',
        [USB_KEY_G] = 'g',
        [USB_KEY_H] = 'h',
        [USB_KEY_I] = 'i',
        [USB_KEY_J] = 'j',
        [USB_KEY_K] = 'k',
        [USB_KEY_L] = 'l',
        [USB_KEY_M] = 'm',
        [USB_KEY_N] = 'n',
        [USB_KEY_O] = 'o',
        [USB_KEY_P] = 'p',
        [USB_KEY_Q] = 'q',
        [USB_KEY_R] = 'r',
        [USB_KEY_S] = 's',
        [USB_KEY_T] = 't',
        [USB_KEY_U] = 'u',
        [USB_KEY_V] = 'v',
        [USB_KEY_W] = 'w',
        [USB_KEY_X] = 'x',
        [USB_KEY_Y] = 'y',
        [USB_KEY_Z] = 'z',
        
        // Numbers
        [USB_KEY_1] = '1',
        [USB_KEY_2] = '2',
        [USB_KEY_3] = '3',
        [USB_KEY_4] = '4',
        [USB_KEY_5] = '5',
        [USB_KEY_6] = '6',
        [USB_KEY_7] = '7',
        [USB_KEY_8] = '8',
        [USB_KEY_9] = '9',
        [USB_KEY_0] = '0',
        
        // Special characters
        [USB_KEY_ENTER] = '\n',
        [USB_KEY_ESCAPE] = 0x1B,
        [USB_KEY_BACKSPACE] = '\b',
        [USB_KEY_TAB] = '\t',
        [USB_KEY_SPACE] = ' ',
        [USB_KEY_MINUS] = '-',
        [USB_KEY_EQUAL] = '=',
        [USB_KEY_LEFTBRACE] = '[',
        [USB_KEY_RIGHTBRACE] = ']',
        [USB_KEY_BACKSLASH] = '\\',
        [USB_KEY_SEMICOLON] = ';',
        [USB_KEY_APOSTROPHE] = '\'',
        [USB_KEY_GRAVE] = '`',
        [USB_KEY_COMMA] = ',',
        [USB_KEY_DOT] = '.',
        [USB_KEY_SLASH] = '/',
        
        // Numpad
        [USB_KEY_KPSLASH] = '/',
        [USB_KEY_KPASTERISK] = '*',
        [USB_KEY_KPMINUS] = '-',
        [USB_KEY_KPPLUS] = '+',
        [USB_KEY_KPENTER] = '\n',
        [USB_KEY_KP1] = '1',
        [USB_KEY_KP2] = '2',
        [USB_KEY_KP3] = '3',
        [USB_KEY_KP4] = '4',
        [USB_KEY_KP5] = '5',
        [USB_KEY_KP6] = '6',
        [USB_KEY_KP7] = '7',
        [USB_KEY_KP8] = '8',
        [USB_KEY_KP9] = '9',
        [USB_KEY_KP0] = '0',
        [USB_KEY_KPDOT] = '.',
    },
    
    // Shift layer (uppercase + symbols)
    .shift = {
        [USB_KEY_NONE] = 0,
        
        // Letters (uppercase)
        [USB_KEY_A] = 'A',
        [USB_KEY_B] = 'B',
        [USB_KEY_C] = 'C',
        [USB_KEY_D] = 'D',
        [USB_KEY_E] = 'E',
        [USB_KEY_F] = 'F',
        [USB_KEY_G] = 'G',
        [USB_KEY_H] = 'H',
        [USB_KEY_I] = 'I',
        [USB_KEY_J] = 'J',
        [USB_KEY_K] = 'K',
        [USB_KEY_L] = 'L',
        [USB_KEY_M] = 'M',
        [USB_KEY_N] = 'N',
        [USB_KEY_O] = 'O',
        [USB_KEY_P] = 'P',
        [USB_KEY_Q] = 'Q',
        [USB_KEY_R] = 'R',
        [USB_KEY_S] = 'S',
        [USB_KEY_T] = 'T',
        [USB_KEY_U] = 'U',
        [USB_KEY_V] = 'V',
        [USB_KEY_W] = 'W',
        [USB_KEY_X] = 'X',
        [USB_KEY_Y] = 'Y',
        [USB_KEY_Z] = 'Z',
        
        // Shifted numbers (symbols)
        [USB_KEY_1] = '!',
        [USB_KEY_2] = '@',
        [USB_KEY_3] = '#',
        [USB_KEY_4] = '$',
        [USB_KEY_5] = '%',
        [USB_KEY_6] = '^',
        [USB_KEY_7] = '&',
        [USB_KEY_8] = '*',
        [USB_KEY_9] = '(',
        [USB_KEY_0] = ')',
        
        // Shifted special characters
        [USB_KEY_MINUS] = '_',
        [USB_KEY_EQUAL] = '+',
        [USB_KEY_LEFTBRACE] = '{',
        [USB_KEY_RIGHTBRACE] = '}',
        [USB_KEY_BACKSLASH] = '|',
        [USB_KEY_SEMICOLON] = ':',
        [USB_KEY_APOSTROPHE] = '"',
        [USB_KEY_GRAVE] = '~',
        [USB_KEY_COMMA] = '<',
        [USB_KEY_DOT] = '>',
        [USB_KEY_SLASH] = '?',
        
        // Control keys (same as normal)
        [USB_KEY_ENTER] = '\n',
        [USB_KEY_ESCAPE] = 0x1B,
        [USB_KEY_BACKSPACE] = '\b',
        [USB_KEY_TAB] = '\t',
        [USB_KEY_SPACE] = ' ',
    },
    
    // AltGr layer (not used in US layout)
    .altgr = {0},
    
    // Shift + AltGr layer (not used in US layout)
    .shift_altgr = {0},
    
    // Ctrl layer (control characters)
    .ctrl = {
        [USB_KEY_A] = 0x01,  // Ctrl+A
        [USB_KEY_B] = 0x02,  // Ctrl+B
        [USB_KEY_C] = 0x03,  // Ctrl+C
        [USB_KEY_D] = 0x04,  // Ctrl+D
        [USB_KEY_E] = 0x05,  // Ctrl+E
        [USB_KEY_F] = 0x06,  // Ctrl+F
        [USB_KEY_G] = 0x07,  // Ctrl+G
        [USB_KEY_H] = 0x08,  // Ctrl+H (backspace)
        [USB_KEY_I] = 0x09,  // Ctrl+I (tab)
        [USB_KEY_J] = 0x0A,  // Ctrl+J (newline)
        [USB_KEY_K] = 0x0B,  // Ctrl+K
        [USB_KEY_L] = 0x0C,  // Ctrl+L (form feed)
        [USB_KEY_M] = 0x0D,  // Ctrl+M (carriage return)
        [USB_KEY_N] = 0x0E,  // Ctrl+N
        [USB_KEY_O] = 0x0F,  // Ctrl+O
        [USB_KEY_P] = 0x10,  // Ctrl+P
        [USB_KEY_Q] = 0x11,  // Ctrl+Q
        [USB_KEY_R] = 0x12,  // Ctrl+R
        [USB_KEY_S] = 0x13,  // Ctrl+S
        [USB_KEY_T] = 0x14,  // Ctrl+T
        [USB_KEY_U] = 0x15,  // Ctrl+U
        [USB_KEY_V] = 0x16,  // Ctrl+V
        [USB_KEY_W] = 0x17,  // Ctrl+W
        [USB_KEY_X] = 0x18,  // Ctrl+X
        [USB_KEY_Y] = 0x19,  // Ctrl+Y
        [USB_KEY_Z] = 0x1A,  // Ctrl+Z
    }
};

#endif // USB_LAYOUT_US_H