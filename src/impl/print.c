#include "print.h"

const static size_t NUM_COLS = 80;
const static size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

struct Char* buffer = (struct Char*) 0xb8000;
size_t col = 0;
size_t row = 0;
uint8_t color = PRINT_COLOR_WHITE | PRINT_COLOR_BLACK << 4;

void clear_row(size_t row) {
    struct Char empty = (struct Char) {
        character: ' ',
        color: color,
    };

    for (size_t col = 0; col < NUM_COLS; col++) {
        buffer[col + NUM_COLS * row] = empty;
    }
}

void print_clear() {
    for (size_t i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }
}

void print_newline() {
    col = 0;

    if (row < NUM_ROWS - 1) {
        row++;
        return;
    }

    for (size_t row = 1; row < NUM_ROWS; row++) {
        for (size_t col = 0; col < NUM_COLS; col++) {
            struct Char character = buffer[col + NUM_COLS * row];
            buffer[col + NUM_COLS * (row - 1)] = character;
        }
    }

    clear_row(NUM_ROWS - 1);
}

void print_char_at(char character, int col, int row) {
    if (col < 0 || col >= NUM_COLS || row < 0 || row >= NUM_ROWS) return;

    buffer[col + NUM_COLS * row] = (struct Char) {
        .character = (uint8_t) character,
        .color = color,
    };
}


void print_char(char character) {
    if (character == '\0') {
        return;
    }

    if (character == '\n') {
        print_newline();
        return;
    }

    if (col >= NUM_COLS) {
        print_newline();
    }

    if (buffer == NULL) {
        return;
    }

    buffer[col + NUM_COLS * row] = (struct Char) {
        .character = (uint8_t) character,
        .color = color,
    };

    col++;
}

void print_str(const char* str) {
    for (size_t i = 0; 1; i++) {
        char character = (uint8_t) str[i];

        if (character == '\0') {
            return;
        }

        print_char(character);
    }
}

void print_set_color(uint8_t foreground, uint8_t background) {
    color = foreground + (background << 4);
}

void print_uint64_dec(uint64_t value) {
    if (value == 0) {
        print_char('0');
        return;
    }
    
    char buffer[20];
    int i = 0;
    
    while (value > 0) {
        buffer[i++] = (value % 10) + '0';
        value /= 10;
    }
    
    while (i-- > 0) {
        print_char(buffer[i]);
    }
}

void print_uint64_hex(uint64_t value) {
    if (value == 0) {
        print_char('0');
        return;
    }
    
    char buffer[16];
    int i = 0;
    
    while (value > 0) {
        uint8_t digit = value & 0xF;
        
        if (digit < 10) {
            buffer[i++] = digit + '0';
        } else {
            buffer[i++] = digit - 10 + 'A';
        }
        
        value >>= 4;
    }
    
    while (i-- > 0) {
        print_char(buffer[i]);
    }
}

void print_uint64_bin(uint64_t value) {
    char buffer[64];
    
    for (size_t i = 0; i < 64; i++) {
        buffer[i] = (value & 1) + '0';
        value >>= 1;
    }
    
    for (size_t i = 64; i > 0; i--) {
        print_char(buffer[i - 1]);
    }
}

void print_str_at(const char* str, int col, int row) {
	volatile uint16_t* vga = (uint16_t*) 0xB8000 + row * 80 + col;
	while (*str) {
		*vga++ = (PRINT_COLOR_WHITE << 8) | *str++;
	}
}

void print_clear_color(uint8_t fg, uint8_t bg) {
	uint16_t value = (bg << 12) | (fg << 8) | ' ';
	for (int i = 0; i < 80 * 25; ++i) {
		((volatile uint16_t*) 0xB8000)[i] = value;
	}
}

void print_int(int64_t value) {
    if (value == 0) {
        print_char('0');
        return;
    }

    if (value < 0) {
        print_char('-');
        value = -value;
    }

    char buffer[20];
    int i = 0;

    while (value > 0) {
        buffer[i++] = (value % 10) + '0';
        value /= 10;
    }

    while (i-- > 0) {
        print_char(buffer[i]);
    }
}

void print_hex64_at(uint64_t value, int col, int row) {
    char hex_digits[] = "0123456789ABCDEF";
    char buffer[16];
    int i = 0;

    if (value == 0) {
        for (int j = 0; j < 16; j++) {
            print_char_at('0', col++, row);
        }
        return;
    }

    // Convert value to hex digits
    while (value > 0 && i < 16) {
        buffer[i++] = hex_digits[value & 0xF];
        value >>= 4;
    }

    // Pad with zeros to make 16 characters
    while (i < 16) {
        buffer[i++] = '0';
    }

    // Print in correct order (most significant digit first)
    while (i-- > 0) {
        print_char_at(buffer[i], col++, row);
    }
}

void print_int_at(int64_t value, int col, int row) {
    char buffer[21];
    int i = 0;

    if (value == 0) {
        print_char_at('0', col, row);
        return;
    }

    int start_col = col;

    if (value < 0) {
        print_char_at('-', col++, row);
        value = -value;
    }

    while (value > 0) {
        buffer[i++] = (value % 10) + '0';
        value /= 10;
    }

    while (i-- > 0) {
        print_char_at(buffer[i], col++, row);
    }
}
