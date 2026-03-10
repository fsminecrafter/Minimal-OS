#include "x86_64/pit.h"
#include "x86_64/port.h"
#include "x86_64/pic.h"
#include "x86_64/interupthandler.h"
#include "x86_64/idt.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "panic.h"

// PIT ports
#define PIT_COMMAND_PORT 0x43
#define PIT_CHANNEL0_DATA_PORT 0x40

// PIT mode and control bits
#define PIT_CHANNEL_0 0x00
#define PIT_ACCESS_LOHI 0x30
#define PIT_MODE_RATE_GENERATOR 0x04

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency = 100;  // Default 100Hz

void pit_init(uint32_t frequency) {
    // Store frequency for time calculations
    pit_frequency = frequency;
    
    // Calculate divisor for PIT input frequency (1.193182 MHz)
    uint16_t divisor = 1193182 / frequency;

    // Send command byte: channel 0, access mode lo/hi, mode 2 (rate generator)
    port_outb(PIT_COMMAND_PORT, PIT_CHANNEL_0 | PIT_ACCESS_LOHI | PIT_MODE_RATE_GENERATOR);

    // Send divisor low byte
    port_outb(PIT_CHANNEL0_DATA_PORT, (uint8_t)(divisor & 0xFF));
    // Send divisor high byte
    port_outb(PIT_CHANNEL0_DATA_PORT, (uint8_t)(divisor >> 8));

    // Enable IRQ0 in PIC (unmask timer interrupt)
    pic_unmask_irq(IRQ0_TIMER);
}

void pit_irq_handler() {
    pit_ticks++;
    
    // Update system time
    time_tick(pit_frequency);

    // Usb poll
    usb_poll();
    
    // Run scheduler
    scheduler_tick();
    
    // Send End Of Interrupt (EOI) to PIC master for IRQ0
    pic_eoi_master();
}

uint64_t pit_get_ticks() {
    return pit_ticks;
}

void setup_kernel_interrupts() {
    idt_init();
    idt_set_handler_pit(pit_irq_handler);
}