#ifndef USB_LAYOUT_SE_H
#define USB_LAYOUT_SE_H

#include "keyboard/usbkeyboard.h"

// Swedish QWERTY Keyboard Layout
// Features: å, ä, ö keys and Swedish-specific symbols

const keyboard_layout_t usb_layout_se_qwerty = {
    .header = {
        .magic = {'K', 'B', 'R', '1'},
        .version = KBR_VERSION,
        .flags = KBR_FLAG_ALTGR,  // Uses AltGr for special characters
        .name = "Swedish QWERTY",
        .language = "sv-SE",
        .author = "MinimalOS",
        .reserved = {0}
    },
    
    // ========================================
    // NORMAL LAYER (no modifiers)
    // ========================================
    .normal = {
        [USB_KEY_NONE] = 0,
        
        // Letters (lowercase) - standard QWERTY
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
        
        // Numbers (Swedish layout)
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
        
        // Special keys
        [USB_KEY_ENTER] = '\n',
        [USB_KEY_ESCAPE] = 0x1B,
        [USB_KEY_BACKSPACE] = '\b',
        [USB_KEY_TAB] = '\t',
        [USB_KEY_SPACE] = ' ',
        
        // Swedish-specific symbol positions
        [USB_KEY_MINUS] = '+',           // Plus on Swedish keyboard
        [USB_KEY_EQUAL] = 0xE9,          // Dead acute accent (´)
        [USB_KEY_LEFTBRACE] = 0xE5,      // å (lower case)
        [USB_KEY_RIGHTBRACE] = 0xA8,     // Diaeresis (¨)
        [USB_KEY_BACKSLASH] = '\'',      // Apostrophe
        [USB_KEY_SEMICOLON] = 0xF6,      // ö (lower case)
        [USB_KEY_APOSTROPHE] = 0xE4,     // ä (lower case)
        [USB_KEY_GRAVE] = 0xA7,          // Section sign (§)
        [USB_KEY_COMMA] = ',',
        [USB_KEY_DOT] = '.',
        [USB_KEY_SLASH] = '-',           // Hyphen/minus
        
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
        [USB_KEY_KPDOT] = ',',          // Swedish uses comma as decimal separator
    },
    
    // ========================================
    // SHIFT LAYER (uppercase + symbols)
    // ========================================
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
        
        // Shifted numbers (Swedish symbols)
        [USB_KEY_1] = '!',
        [USB_KEY_2] = '"',           // Double quote (not @)
        [USB_KEY_3] = '#',
        [USB_KEY_4] = 0xA4,          // Currency sign (¤)
        [USB_KEY_5] = '%',
        [USB_KEY_6] = '&',
        [USB_KEY_7] = '/',           // Forward slash
        [USB_KEY_8] = '(',
        [USB_KEY_9] = ')',
        [USB_KEY_0] = '=',
        
        // Shifted special characters
        [USB_KEY_MINUS] = '?',           // Question mark
        [USB_KEY_EQUAL] = '`',           // Grave accent
        [USB_KEY_LEFTBRACE] = 0xC5,      // Å (upper case)
        [USB_KEY_RIGHTBRACE] = '^',      // Caret
        [USB_KEY_BACKSLASH] = '*',       // Asterisk
        [USB_KEY_SEMICOLON] = 0xD6,      // Ö (upper case)
        [USB_KEY_APOSTROPHE] = 0xC4,     // Ä (upper case)
        [USB_KEY_GRAVE] = 0xBD,          // One half (½)
        [USB_KEY_COMMA] = ';',
        [USB_KEY_DOT] = ':',
        [USB_KEY_SLASH] = '_',           // Underscore
        
        // Control keys (same as normal)
        [USB_KEY_ENTER] = '\n',
        [USB_KEY_ESCAPE] = 0x1B,
        [USB_KEY_BACKSPACE] = '\b',
        [USB_KEY_TAB] = '\t',
        [USB_KEY_SPACE] = ' ',
    },
    
    // ========================================
    // ALTGR LAYER (special characters)
    // ========================================
    .altgr = {
        [USB_KEY_NONE] = 0,
        
        // AltGr + numbers for special characters
        [USB_KEY_2] = '@',           // AltGr+2 = @
        [USB_KEY_3] = 0xA3,          // AltGr+3 = £ (pound)
        [USB_KEY_4] = '$',           // AltGr+4 = $
        [USB_KEY_5] = 0x80,          // AltGr+5 = € (euro)
        [USB_KEY_7] = '{',           // AltGr+7 = {
        [USB_KEY_8] = '[',           // AltGr+8 = [
        [USB_KEY_9] = ']',           // AltGr+9 = ]
        [USB_KEY_0] = '}',           // AltGr+0 = }
        
        // AltGr + special keys
        [USB_KEY_MINUS] = '\\',          // AltGr++ = backslash
        [USB_KEY_RIGHTBRACE] = '~',      // AltGr+¨ = tilde
        [USB_KEY_BACKSLASH] = '*',       // AltGr+' = asterisk
        [USB_KEY_GRAVE] = '|',           // AltGr+§ = pipe
        
        // AltGr + letters for special chars
        [USB_KEY_E] = 0x80,          // AltGr+E = € (euro)
        [USB_KEY_M] = 0xB5,          // AltGr+M = µ (micro)
    },
    
    // ========================================
    // SHIFT + ALTGR LAYER
    // ========================================
    .shift_altgr = {
        [USB_KEY_NONE] = 0,
        
        // Additional special characters with Shift+AltGr
        [USB_KEY_BACKSLASH] = '*',   // Shift+AltGr+' = asterisk
    },
    
    // ========================================
    // CTRL LAYER (control characters)
    // ========================================
    .ctrl = {
        [USB_KEY_A] = 0x01,  // Ctrl+A
        [USB_KEY_B] = 0x02,  // Ctrl+B
        [USB_KEY_C] = 0x03,  // Ctrl+C (copy/break)
        [USB_KEY_D] = 0x04,  // Ctrl+D
        [USB_KEY_E] = 0x05,  // Ctrl+E
        [USB_KEY_F] = 0x06,  // Ctrl+F
        [USB_KEY_G] = 0x07,  // Ctrl+G (bell)
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
        [USB_KEY_S] = 0x13,  // Ctrl+S (save)
        [USB_KEY_T] = 0x14,  // Ctrl+T
        [USB_KEY_U] = 0x15,  // Ctrl+U
        [USB_KEY_V] = 0x16,  // Ctrl+V (paste)
        [USB_KEY_W] = 0x17,  // Ctrl+W
        [USB_KEY_X] = 0x18,  // Ctrl+X (cut)
        [USB_KEY_Y] = 0x19,  // Ctrl+Y
        [USB_KEY_Z] = 0x1A,  // Ctrl+Z (undo)
    }
};

// ===========================================
// SWEDISH KEYBOARD LAYOUT NOTES
// ===========================================

/*
The Swedish keyboard layout differs from US in several ways:

1. SPECIAL CHARACTERS:
   - å, ä, ö letters have dedicated keys
   - § (section) instead of `
   - + instead of = (unshifted)
   - - instead of / (unshifted)

2. NUMBER ROW SYMBOLS (with Shift):
   - Shift+2 = " (not @)
   - Shift+4 = ¤ (currency, not $)
   - Shift+6 = & (not ^)
   - Shift+7 = / (not &)
   - Shift+8 = ( (not *)
   - Shift+0 = = (not ))

3. ALTGR LAYER:
   - AltGr+2 = @
   - AltGr+3 = £
   - AltGr+4 = $
   - AltGr+5 = €
   - AltGr+7/8/9/0 = { [ ] }
   - AltGr+E = € (euro sign)

4. PUNCTUATION:
   - . and : are on the same key
   - , and ; are on the same key
   - Decimal separator is comma (,) not period

5. UNICODE CHARACTERS:
   å = 0xE5 (lowercase), Å = 0xC5 (uppercase)
   ä = 0xE4 (lowercase), Ä = 0xC4 (uppercase)
   ö = 0xF6 (lowercase), Ö = 0xD6 (uppercase)
   € = 0x80 (euro sign)
   § = 0xA7 (section sign)
   ½ = 0xBD (one half)
   ¤ = 0xA4 (currency sign)
   µ = 0xB5 (micro sign)
*/

#endif // USB_LAYOUT_SE_H