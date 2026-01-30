#include "string.h"
#include <stdint.h>

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

__attribute__((optimize("O2")))
void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    unsigned char byte = (unsigned char)c;
    
    // Use rep stosq for maximum performance on x86-64
    if (n >= 8 && ((uintptr_t)p & 7) == 0 && byte == 0) {
        // Aligned and zeroing - use rep stosq
        size_t qwords = n / 8;
        size_t bytes_written = qwords * 8;
        
        __asm__ volatile(
            "rep stosq"
            : "+D"(p), "+c"(qwords)
            : "a"(0ULL)
            : "memory"
        );
        
        // p now points past the last qword written
        // Adjust remaining byte count
        n -= bytes_written;
    }
    
    // Handle remaining bytes or non-zero patterns
    while (n--) {
        *p++ = byte;
    }
    
    return s;
}

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
	// Reverse into output
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

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) {
        *d++ = *s++;
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

    // Skip leading delimiters
    while (*next && strchr(delim, *next)) {
        next++;
    }

    if (*next == '\0') {
        return NULL;
    }

    char* token_start = next;

    // Find end of token
    while (*next && !strchr(delim, *next)) {
        next++;
    }

    if (*next) {
        *next = '\0';
        next++;
    }

    return token_start;
}