#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include "x86_64/rtc.h"

// x86 I/O port operations

static inline void port_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t port_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void port_outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t port_inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void port_outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t port_inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
void port_wait();

static uint8_t binary_to_bcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

// Read from CMOS register
static uint8_t cmos_read(uint8_t reg) {
    port_outb(CMOS_ADDRESS, reg);
    return port_inb(CMOS_DATA);
}

// Check if RTC is updating
static int rtc_is_updating() {
    port_outb(CMOS_ADDRESS, RTC_STATUS_A);
    return (port_inb(CMOS_DATA) & 0x80);
}

// Convert BCD to binary if needed
static uint8_t bcd_to_binary(uint8_t bcd) {
    return ((bcd & 0xF0) >> 4) * 10 + (bcd & 0x0F);
}

// Read RTC value with BCD conversion check
static uint8_t rtc_read_reg(uint8_t reg) {
    // Wait for update to complete
    while (rtc_is_updating());
    
    uint8_t value = cmos_read(reg);
    
    // Check if RTC is in BCD mode
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    if (!(status_b & 0x04)) {
        // BCD mode - convert to binary
        value = bcd_to_binary(value);
    }
    
    return value;
}

static void cmos_write(uint8_t reg, uint8_t value) {
    port_outb(CMOS_ADDRESS, reg | 0x80); // disable NMI
    port_outb(CMOS_DATA, value);
}

static void rtc_write_reg(uint8_t reg, uint8_t value) {

    while (rtc_is_updating());

    uint8_t status_b = cmos_read(RTC_STATUS_B);

    // Convert to BCD if needed
    if (!(status_b & 0x04)) {
        value = binary_to_bcd(value);
    }

    cmos_write(reg, value);

    // -------- Verification --------
    for (int i = 0; i < 3; i++) {

        while (rtc_is_updating());

        uint8_t check = rtc_read_reg(reg);

        if (check == value || check == bcd_to_binary(value))
            return;

        cmos_write(reg, value);
    }
}

// Alternative inline version
static inline void port_io_wait(void) {
    port_outb(0x80, 0);  // Write to unused port
}

// ===========================================
// HELPER MACROS
// ===========================================

// Read-modify-write helpers
#define PORT_SET_BIT(port, bit) \
    port_outb(port, port_inb(port) | (1 << (bit)))

#define PORT_CLEAR_BIT(port, bit) \
    port_outb(port, port_inb(port) & ~(1 << (bit)))

#define PORT_TOGGLE_BIT(port, bit) \
    port_outb(port, port_inb(port) ^ (1 << (bit)))

#endif // PORT_H