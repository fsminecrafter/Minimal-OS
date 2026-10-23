#include "serial.h"
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/smp.h"

bool enabled = true;
bool initialized = false;

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
    if (!enabled) return;
    initialized = true;
    outb(SERIAL_COM1 + 1, 0x00); // Disable interrupts
    outb(SERIAL_COM1 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(SERIAL_COM1 + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(SERIAL_COM1 + 1, 0x00); //                  (hi byte)
    outb(SERIAL_COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(SERIAL_COM1 + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(SERIAL_COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

int serial_received() {
    if (!initialized) return 0;
    return inb(SERIAL_COM1 + 5) & 1;
}

char serial_read() {
    if (!initialized || !enabled) return 0;
    while (serial_received() == 0);
    return (char)inb(SERIAL_COM1);
}

int serial_is_transmit_empty() {
    if (!initialized) return 0;
    return inb(SERIAL_COM1 + 5) & 0x20;
}

/*
 * One line at a time: a core keeps COM1 from its first character to its
 * '\n', so lines printed by several cores at once do not mix character by
 * character (a line is usually several calls: text, a number, "\n").
 * Interrupts and other processes on the same core just continue the line.
 * A core that stops mid-line (halted, or never ends the line) loses COM1
 * to the next one after about 50 ms.
 */
#define SERIAL_NO_OWNER 0xFFFFFFFFu
static volatile uint32_t g_line_owner = SERIAL_NO_OWNER;

static void serial_take_line(uint32_t cpu) {
    if (g_line_owner == cpu) return;
    for (uint32_t spins = 0; spins < 2000000u; spins++) {
        uint32_t expected = SERIAL_NO_OWNER;
        if (__atomic_compare_exchange_n(&g_line_owner, &expected, cpu, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        asm volatile("pause");
    }
    __atomic_store_n(&g_line_owner, cpu, __ATOMIC_RELEASE);   // stalled owner
}

void serial_write(char a) {
    if (!initialized) return;
    if (!enabled) return;
    uint32_t cpu = smp_current_cpu_id();
    serial_take_line(cpu);
    while (serial_is_transmit_empty() == 0);
    outb(SERIAL_COM1, a);
    if (a == '\n') __atomic_store_n(&g_line_owner, SERIAL_NO_OWNER, __ATOMIC_RELEASE);
}

void serial_write_str(const char* str) {
    if (!initialized) return;
    if (!enabled) return;
    while (*str) {
        if (*str == '\n') {
            serial_write('\r'); // Add carriage return before newline
        }
        serial_write(*str++);
    }
}

// Write decimal representation of number
void serial_write_dec(uint64_t val) {
    if (!initialized) return;
    if (!enabled) return;
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
    if (!initialized) return;
    if (!enabled) return;
    char hex[] = "0123456789ABCDEF";
    serial_write_str("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        serial_write(hex[nibble]);
    }
}

// Write binary representation of number
void serial_write_bin(uint64_t val) {
    if (!enabled) return;
    if (!initialized) return;
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
