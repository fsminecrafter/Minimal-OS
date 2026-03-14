#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ===========================================
// TRADITIONAL C STRING FUNCTIONS
// ===========================================

size_t strlen(const char* str);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);

char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
void itoa(int value, char* str, int base);

void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);

char* strtok(char* str, const char* delim);

// Conversion functions
void uint_to_str(uint64_t value, char* out);
void int_to_str(int64_t value, char* out);
void hex_to_str(uint64_t value, char* out);
void bin_to_str(uint64_t value, char* out);
void float_to_str(double value, char* out, int precision);

// ===========================================
// ENHANCED STRING TYPE
// ===========================================

#define STRING_MAX_LENGTH 256

// Managed string type with automatic length tracking
typedef struct {
    char data[STRING_MAX_LENGTH];
    size_t length;
    size_t capacity;
} string_t;

// String initialization
void string_init(string_t* str);
void string_init_cstr(string_t* str, const char* cstr);
void string_clear(string_t* str);

// String operations
void string_append(string_t* str, const char* cstr);
void string_append_char(string_t* str, char c);
void string_append_str(string_t* str, const string_t* other);
void string_copy(string_t* dest, const string_t* src);
void string_copy_cstr(string_t* dest, const char* cstr);

// String comparison
bool string_equals(const string_t* s1, const string_t* s2);
bool string_equals_cstr(const string_t* str, const char* cstr);
int string_compare(const string_t* s1, const string_t* s2);

// String searching
int string_find(const string_t* str, const char* substr);
int string_find_char(const string_t* str, char c);
bool string_contains(const string_t* str, const char* substr);
bool string_starts_with(const string_t* str, const char* prefix);
bool string_ends_with(const string_t* str, const char* suffix);

// String manipulation
void string_substring(string_t* dest, const string_t* src, size_t start, size_t length);
void string_trim(string_t* str);
void string_to_upper(string_t* str);
void string_to_lower(string_t* str);
void string_replace_char(string_t* str, char old_char, char new_char);

// String conversion to other types
int64_t string_to_int(const string_t* str);
uint64_t string_to_uint(const string_t* str);
double string_to_float(const string_t* str);
bool string_to_bool(const string_t* str);

// Get C string pointer
const char* string_cstr(const string_t* str);

// ===========================================
// UNIVERSAL CONVERSION FUNCTIONS (str/int)
// ===========================================

// STR() - Convert any type to string_t
// Usage: str_from_int(42), str_from_float(3.14), etc.

string_t str_from_int(int64_t value);
string_t str_from_uint(uint64_t value);
string_t str_from_float(double value, int precision);
string_t str_from_bool(bool value);
string_t str_from_char(char c);
string_t str_from_cstr(const char* cstr);
string_t str_from_hex(uint64_t value);
string_t str_from_bin(uint64_t value);

// Convert to C string (static buffer - use immediately!)
const char* str_int(int64_t value);
const char* str_uint(uint64_t value);
const char* str_float(double value, int precision);
const char* str_hex(uint64_t value);
const char* str_bin(uint64_t value);

// INT() - Convert any type to int64_t
int64_t int_from_str(const string_t* str);
int64_t int_from_cstr(const char* str);
int64_t int_from_float(double value);
int64_t int_from_bool(bool value);
int64_t int_from_char(char c);
int64_t int_from_hex(const char* hex_str);

// UINT() - Convert to uint64_t
uint64_t uint_from_str(const string_t* str);
uint64_t uint_from_cstr(const char* str);
uint64_t uint_from_int(int64_t value);
uint64_t uint_from_float(double value);

// FLOAT() - Convert to double
double float_from_str(const string_t* str);
double float_from_cstr(const char* str);
double float_from_int(int64_t value);
double float_from_uint(uint64_t value);

// BOOL() - Convert to bool
bool bool_from_str(const string_t* str);
bool bool_from_cstr(const char* str);
bool bool_from_int(int64_t value);

// ===========================================
// ARRAY/LIST STRING CONVERSION
// ===========================================

// Convert array to string representation
void str_from_int_array(string_t* dest, const int64_t* array, size_t count);
void str_from_uint_array(string_t* dest, const uint64_t* array, size_t count);
void str_from_float_array(string_t* dest, const double* array, size_t count, int precision);
void str_from_str_array(string_t* dest, const char** array, size_t count);

// Format: "[1, 2, 3, 4, 5]"
const char* str_int_array(const int64_t* array, size_t count);
const char* str_uint_array(const uint64_t* array, size_t count);

// ===========================================
// FORMATTING & UTILITIES
// ===========================================

// Simplified sprintf-like function
void string_format(string_t* dest, const char* format, ...);

// String building helpers
void string_append_int(string_t* str, int64_t value);
void string_append_uint(string_t* str, uint64_t value);
void string_append_hex(string_t* str, uint64_t value);
void string_append_float(string_t* str, double value, int precision);

// Parsing helpers
bool str_parse_int(const char* str, int64_t* out);
bool str_parse_uint(const char* str, uint64_t* out);
bool str_parse_float(const char* str, double* out);

// Character classification
bool is_digit(char c);
bool is_alpha(char c);
bool is_alnum(char c);
bool is_space(char c);
bool is_upper(char c);
bool is_lower(char c);

char to_upper(char c);
char to_lower(char c);

// ===========================================
// CONVENIENCE MACROS
// ===========================================

// Easy conversion macros
#define STR_INT(x)    str_from_int(x)
#define STR_UINT(x)   str_from_uint(x)
#define STR_FLOAT(x)  str_from_float(x, 2)
#define STR_HEX(x)    str_from_hex(x)
#define STR_BOOL(x)   str_from_bool(x)
#define STR(x)        str_from_cstr(x)

#define INT(x)        int_from_cstr(x)
#define UINT(x)       uint_from_cstr(x)
#define FLOAT(x)      float_from_cstr(x)
#define BOOL(x)       bool_from_cstr(x)

// String literal creation
#define STRING(cstr)  ({ string_t s; string_init_cstr(&s, cstr); s; })

#endif // STRING_H