#include <stdint.h>
#include "x86_64/port.h"
#include "print.h"

#define PIT_COMMAND_PORT 0x43
#define PIT_CHANNEL2_DATA_PORT 0x42

static void play_sound(uint32_t frequency) {
    if (frequency == 0) return;  // avoid div by zero

    uint32_t divisor = 1193180 / frequency;
    uint8_t tmp;

    // Set PIT channel 2 to mode 3 (square wave generator), access mode lobyte/hibyte
    // Command byte: 10110110b = 0xB6
    port_outb(PIT_COMMAND_PORT, 0xB6);

    // Send frequency divisor low byte and high byte to channel 2 data port
    port_outb(PIT_CHANNEL2_DATA_PORT, (uint8_t)(divisor & 0xFF));
    port_outb(PIT_CHANNEL2_DATA_PORT, (uint8_t)(divisor >> 8));

    // Enable PC speaker via port 0x61 (set bits 0 and 1)
    tmp = port_inb(0x61);
    if ((tmp & 0x03) != 0x03) {
        port_outb(0x61, tmp | 0x03);
    }
}

static void nosound() {
    uint8_t tmp = port_inb(0x61) & 0xFC; // clear bits 0 and 1 (disable speaker and gate)
    port_outb(0x61, tmp);
}

void beep() {
    play_sound(1000);  // 1 kHz tone
    int i;
    for (i = 100000000; i--; i == 0) {
        asm volatile ("nop"); //for now
    }
    nosound();
}
