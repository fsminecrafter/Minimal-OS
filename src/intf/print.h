#pragma once

#include <stdint.h>
#include <stddef.h>

enum {
    PRINT_COLOR_BLACK = 0,
	PRINT_COLOR_BLUE = 1,
	PRINT_COLOR_GREEN = 2,
	PRINT_COLOR_CYAN = 3,
	PRINT_COLOR_RED = 4,
	PRINT_COLOR_MAGENTA = 5,
	PRINT_COLOR_BROWN = 6,
	PRINT_COLOR_LIGHT_GRAY = 7,
	PRINT_COLOR_DARK_GRAY = 8,
	PRINT_COLOR_LIGHT_BLUE = 9,
	PRINT_COLOR_LIGHT_GREEN = 10,
	PRINT_COLOR_LIGHT_CYAN = 11,
	PRINT_COLOR_LIGHT_RED = 12,
	PRINT_COLOR_PINK = 13,
	PRINT_COLOR_YELLOW = 14,
	PRINT_COLOR_WHITE = 15,
};

void print_clear();
void print_char(char character);
void print_str(char* string);
void print_set_color(uint8_t foreground, uint8_t background);
void print_uint64_dec(uint64_t value);
void print_uint64_hex(uint64_t value);
void print_uint64_bin(uint64_t value);
void print_clear_color(uint8_t fg, uint8_t bg);
void print_str_at(const char* str, int col, int row);
void print_char_at(char character, int col, int row);
void print_int_at(int64_t value, int col, int row);
void print_int(int64_t value);
void print_hex64_at(uint64_t value, int col, int row);