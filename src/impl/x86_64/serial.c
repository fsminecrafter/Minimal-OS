#include "serial.h"
#include <stdint.h>

// Write a byte to an I/O port
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

// Read a byte from an I/O port
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init() {
    outb(SERIAL_COM1 + 1, 0x00); // Disable interrupts
    outb(SERIAL_COM1 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(SERIAL_COM1 + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(SERIAL_COM1 + 1, 0x00); //                  (hi byte)
    outb(SERIAL_COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(SERIAL_COM1 + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(SERIAL_COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

int serial_received() {
    return inb(SERIAL_COM1 + 5) & 1;
}

char serial_read() {
    while (serial_received() == 0);
    return inb(SERIAL_COM1);
}

int serial_is_transmit_empty() {
    return inb(SERIAL_COM1 + 5) & 0x20;
}

void serial_write(char a) {
    while (serial_is_transmit_empty() == 0);
    outb(SERIAL_COM1, a);
}

void serial_write_str(const char* str) {
    while (*str) {
        if (*str == '\n') {
            serial_write('\r'); // Add carriage return before newline
        }
        serial_write(*str++);
    }
}

// Write decimal representation of number
void serial_write_dec(uint64_t val) {
    char buffer[21];
    int i = 20;
    buffer[i--] = '\0';
    if (val == 0) {
        serial_write('0');
        return;
    }
    while (val > 0 && i >= 0) {
        buffer[i--] = '0' + (val % 10);
        val /= 10;
    }
    serial_write_str(&buffer[i + 1]);
}

// Write hexadecimal representation of number
void serial_write_hex(uint64_t val) {
    char hex[] = "0123456789ABCDEF";
    serial_write_str("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        serial_write(hex[nibble]);
    }
}

// Write binary representation of number
void serial_write_bin(uint64_t val) {
    serial_write_str("0b");
    int started = 0;
    for (int i = 63; i >= 0; i--) {
        char bit = ((val >> i) & 1) ? '1' : '0';
        if (!started && bit == '0' && i != 0) continue;
        started = 1;
        serial_write(bit);
    }
    if (!started) serial_write('0');
}