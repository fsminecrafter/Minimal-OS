#include "string.h"
#include <stdint.h>
#include <stdarg.h>

// ===========================================
// TRADITIONAL STRING FUNCTIONS (existing code)
// ===========================================

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* strcpy(char* dest, const char* src) {
    char* r = dest;
    while ((*dest++ = *src++));
    return r;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* r = dest;
    while (n && *src) {
        *dest++ = *src++;
        n--;
    }
    while (n--) {
        *dest++ = '\0';
    }
    return r;
}

char* strcat(char* dest, const char* src) {
    char* r = dest;
    while (*dest) dest++;
    while ((*dest++ = *src++));
    return r;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char*)s;
        s++;
    }
    return NULL;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    unsigned char byte = (unsigned char)c;
    
    if (byte == 0 && n >= 8 && ((uintptr_t)p & 7) == 0) {
        uint64_t* p64 = (uint64_t*)p;
        size_t qwords = n / 8;
        
        for (size_t i = 0; i < qwords; i++) {
            p64[i] = 0;
        }
        
        p = (unsigned char*)(p64 + qwords);
        n = n & 7;
    }
    
    for (size_t i = 0; i < n; i++) {
        p[i] = byte;
    }
    
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = s1;
    const unsigned char* p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

char* strtok(char* str, const char* delim) {
    static char* next;
    if (str) {
        next = str;
    }
    if (!next || *next == '\0') {
        return NULL;
    }

    while (*next && strchr(delim, *next)) {
        next++;
    }

    if (*next == '\0') {
        return NULL;
    }

    char* token_start = next;

    while (*next && !strchr(delim, *next)) {
        next++;
    }

    if (*next) {
        *next = '\0';
        next++;
    }

    return token_start;
}

// ===========================================
// CONVERSION HELPERS
// ===========================================

void hex_to_str(uint64_t value, char* out) {
    static const char* digits = "0123456789ABCDEF";
    int i = 0;
    char tmp[17];
    tmp[16] = '\0';

    if (value == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }

    while (value && i < 16) {
        tmp[i++] = digits[value & 0xF];
        value >>= 4;
    }
    
    for (int j = 0; j < i; j++) {
        out[j] = tmp[i - j - 1];
    }
    out[i] = '\0';
}

void uint_to_str(uint64_t value, char* out) {
    char tmp[21];
    int i = 0;

    if (value == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }

    while (value) {
        tmp[i++] = '0' + (value % 10);
        value /= 10;
    }

    for (int j = 0; j < i; j++) {
        out[j] = tmp[i - j - 1];
    }
    out[i] = '\0';
}

void int_to_str(int64_t value, char* out) {
    if (value < 0) {
        *out++ = '-';
        value = -value;
    }
    uint_to_str((uint64_t)value, out);
}

void bin_to_str(uint64_t value, char* out) {
    if (value == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    
    int i = 0;
    char tmp[65];
    while (value) {
        tmp[i++] = (value & 1) ? '1' : '0';
        value >>= 1;
    }
    
    for (int j = 0; j < i; j++) {
        out[j] = tmp[i - j - 1];
    }
    out[i] = '\0';
}

void float_to_str(double value, char* out, int precision) {
    if (value < 0) {
        *out++ = '-';
        value = -value;
    }
    
    int64_t int_part = (int64_t)value;
    double frac_part = value - int_part;
    
    uint_to_str(int_part, out);
    while (*out) out++;
    
    if (precision > 0) {
        *out++ = '.';
        
        for (int i = 0; i < precision; i++) {
            frac_part *= 10;
            int digit = (int)frac_part;
            *out++ = '0' + digit;
            frac_part -= digit;
        }
    }
    *out = '\0';
}

// ===========================================
// STRING_T IMPLEMENTATION
// ===========================================

void string_init(string_t* str) {
    str->data[0] = '\0';
    str->length = 0;
    str->capacity = STRING_MAX_LENGTH;
}

void string_init_cstr(string_t* str, const char* cstr) {
    string_init(str);
    if (cstr) {
        size_t len = strlen(cstr);
        if (len >= STRING_MAX_LENGTH) {
            len = STRING_MAX_LENGTH - 1;
        }
        memcpy(str->data, cstr, len);
        str->data[len] = '\0';
        str->length = len;
    }
}

void string_clear(string_t* str) {
    str->data[0] = '\0';
    str->length = 0;
}

void string_append(string_t* str, const char* cstr) {
    if (!cstr) return;
    
    size_t add_len = strlen(cstr);
    size_t available = str->capacity - str->length - 1;
    
    if (add_len > available) {
        add_len = available;
    }
    
    memcpy(str->data + str->length, cstr, add_len);
    str->length += add_len;
    str->data[str->length] = '\0';
}

void string_append_char(string_t* str, char c) {
    if (str->length < str->capacity - 1) {
        str->data[str->length++] = c;
        str->data[str->length] = '\0';
    }
}

void string_append_str(string_t* str, const string_t* other) {
    string_append(str, other->data);
}

void string_copy(string_t* dest, const string_t* src) {
    memcpy(dest->data, src->data, src->length + 1);
    dest->length = src->length;
}

void string_copy_cstr(string_t* dest, const char* cstr) {
    string_clear(dest);
    string_append(dest, cstr);
}

bool string_equals(const string_t* s1, const string_t* s2) {
    return strcmp(s1->data, s2->data) == 0;
}

bool string_equals_cstr(const string_t* str, const char* cstr) {
    return strcmp(str->data, cstr) == 0;
}

int string_compare(const string_t* s1, const string_t* s2) {
    return strcmp(s1->data, s2->data);
}

int string_find(const string_t* str, const char* substr) {
    char* pos = strstr(str->data, substr);
    return pos ? (int)(pos - str->data) : -1;
}

int string_find_char(const string_t* str, char c) {
    char* pos = strchr(str->data, c);
    return pos ? (int)(pos - str->data) : -1;
}

bool string_contains(const string_t* str, const char* substr) {
    return string_find(str, substr) >= 0;
}

bool string_starts_with(const string_t* str, const char* prefix) {
    size_t prefix_len = strlen(prefix);
    if (prefix_len > str->length) return false;
    return strncmp(str->data, prefix, prefix_len) == 0;
}

bool string_ends_with(const string_t* str, const char* suffix) {
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str->length) return false;
    return strcmp(str->data + str->length - suffix_len, suffix) == 0;
}

const char* string_cstr(const string_t* str) {
    return str->data;
}

// ===========================================
// UNIVERSAL CONVERSION FUNCTIONS
// ===========================================

string_t str_from_int(int64_t value) {
    string_t s;
    string_init(&s);
    char buf[32];
    int_to_str(value, buf);
    string_append(&s, buf);
    return s;
}

string_t str_from_uint(uint64_t value) {
    string_t s;
    string_init(&s);
    char buf[32];
    uint_to_str(value, buf);
    string_append(&s, buf);
    return s;
}

string_t str_from_float(double value, int precision) {
    string_t s;
    string_init(&s);
    char buf[64];
    float_to_str(value, buf, precision);
    string_append(&s, buf);
    return s;
}

string_t str_from_bool(bool value) {
    string_t s;
    string_init_cstr(&s, value ? "true" : "false");
    return s;
}

string_t str_from_char(char c) {
    string_t s;
    string_init(&s);
    string_append_char(&s, c);
    return s;
}

string_t str_from_cstr(const char* cstr) {
    string_t s;
    string_init_cstr(&s, cstr);
    return s;
}

string_t str_from_hex(uint64_t value) {
    string_t s;
    string_init(&s);
    char buf[32];
    hex_to_str(value, buf);
    string_append(&s, "0x");
    string_append(&s, buf);
    return s;
}

// Static buffers for quick conversions (use immediately!)
static char conv_buf[4][256];
static int conv_idx = 0;

const char* str_int(int64_t value) {
    char* buf = conv_buf[conv_idx++ % 4];
    int_to_str(value, buf);
    return buf;
}

const char* str_uint(uint64_t value) {
    char* buf = conv_buf[conv_idx++ % 4];
    uint_to_str(value, buf);
    return buf;
}

const char* str_hex(uint64_t value) {
    char* buf = conv_buf[conv_idx++ % 4];
    hex_to_str(value, buf);
    return buf;
}

// INT conversions
int64_t int_from_cstr(const char* str) {
    if (!str) return 0;
    
    int64_t result = 0;
    bool negative = false;
    
    while (is_space(*str)) str++;
    
    if (*str == '-') {
        negative = true;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (is_digit(*str)) {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return negative ? -result : result;
}

int64_t int_from_str(const string_t* str) {
    return int_from_cstr(str->data);
}

int64_t int_from_float(double value) {
    return (int64_t)value;
}

int64_t int_from_bool(bool value) {
    return value ? 1 : 0;
}

int64_t int_from_char(char c) {
    return (int64_t)c;
}

// Character classification
bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}

char to_upper(char c) {
    return is_lower(c) ? c - 32 : c;
}

char to_lower(char c) {
    return is_upper(c) ? c + 32 : c;
}

// Array conversions
void str_from_int_array(string_t* dest, const int64_t* array, size_t count) {
    string_init(dest);
    string_append(dest, "[");
    
    for (size_t i = 0; i < count; i++) {
        if (i > 0) string_append(dest, ", ");
        char buf[32];
        int_to_str(array[i], buf);
        string_append(dest, buf);
    }
    
    string_append(dest, "]");
}

const char* str_int_array(const int64_t* array, size_t count) {
    static string_t s;
    str_from_int_array(&s, array, count);
    return s.data;
}

void itoa(int value, char* str, int base) {
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    int tmp_value;

    if (value == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }

    while (value != 0) {
        tmp_value = value % base;
        *ptr++ = (tmp_value < 10) ? (tmp_value + '0') : (tmp_value - 10 + 'a');
        value /= base;
    }

    *ptr-- = '\0';

    // Reverse string
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

static size_t append_to_buffer(char* buf, size_t bufsize, size_t pos, const char* src) {
    while (*src && pos + 1 < bufsize) {
        buf[pos++] = *src++;
    }
    return pos;
}

static size_t append_char_to_buffer(char* buf, size_t bufsize, size_t pos, char c) {
    if (pos + 1 < bufsize) {
        buf[pos++] = c;
    }
    return pos;
}

// ------------------------
// Minimal vsnprintf implementation
// ------------------------
static int vsnprintf_internal(char* buffer, size_t size, const char* fmt, va_list args) {
    size_t pos = 0;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == '\0') break;

            bool longlong = false;

            if (*fmt == 'l') {
                fmt++;
                if (*fmt == 'l') {
                    longlong = true;
                    fmt++;
                }
            }

            switch (*fmt) {
                case 's': {
                    const char* s = va_arg(args, const char*);
                    if (!s) s = "(null)";
                    pos = append_to_buffer(buffer, size, pos, s);
                    break;
                }

                case 'c': {
                    char c = (char)va_arg(args, int);
                    pos = append_char_to_buffer(buffer, size, pos, c);
                    break;
                }

                case 'd':
                case 'i': {
                    int64_t val = longlong ? va_arg(args, int64_t) : va_arg(args, int);
                    char tmp[32];
                    int_to_str(val, tmp);
                    pos = append_to_buffer(buffer, size, pos, tmp);
                    break;
                }

                case 'u': {
                    uint64_t val = longlong ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                    char tmp[32];
                    uint_to_str(val, tmp);
                    pos = append_to_buffer(buffer, size, pos, tmp);
                    break;
                }

                case 'x':
                case 'X': {
                    uint64_t val = longlong ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                    char tmp[32];
                    hex_to_str(val, tmp);
                    pos = append_to_buffer(buffer, size, pos, tmp);
                    break;
                }

                case 'p': {
                    uint64_t val = (uint64_t)va_arg(args, void*);
                    char tmp[32];
                    hex_to_str(val, tmp);
                    pos = append_to_buffer(buffer, size, pos, "0x");
                    pos = append_to_buffer(buffer, size, pos, tmp);
                    break;
                }

                case '%': {
                    pos = append_char_to_buffer(buffer, size, pos, '%');
                    break;
                }

                default: {
                    pos = append_char_to_buffer(buffer, size, pos, '%');
                    pos = append_char_to_buffer(buffer, size, pos, *fmt);
                    break;
                }
            }
        } else {
            pos = append_char_to_buffer(buffer, size, pos, *fmt);
        }
        fmt++;
    }

    if (size > 0) {
        buffer[pos < size ? pos : size - 1] = '\0';
    }

    return (int)pos; // number of chars that would have been written
}

// ------------------------
// Public functions
// ------------------------
int snprintf(char* buffer, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf_internal(buffer, size, fmt, args);
    va_end(args);
    return ret;
}

int sprintf(char* buffer, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    // Use a huge buffer size to simulate "unbounded" sprintf
    int ret = vsnprintf_internal(buffer, (size_t)-1, fmt, args);
    va_end(args);
    return ret;
}

int vsnprintf(char* buffer, size_t size, const char* fmt, va_list args) {
    return vsnprintf_internal(buffer, size, fmt, args);
}