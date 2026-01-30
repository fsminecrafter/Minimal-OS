#include <stdint.h>
#include "x86_64/port.h"
#include "x86_64/pic.h"

void pic_eoi_master() {
	port_outb(PORT_PIC1_COMMAND, PIC_EOI);
	port_wait();
}

void pic_eoi_slave() {
	port_outb(PORT_PIC2_COMMAND, PIC_EOI);
	port_wait();
}


void pic_remap() {
    // ICW1 - begin initialization sequence
    port_outb(PORT_PIC1_COMMAND, ICW1_INIT | ICW1_HAS_ICW4); // master
    port_wait();
    port_outb(PORT_PIC2_COMMAND, ICW1_INIT | ICW1_HAS_ICW4); // slave
    port_wait();

    // ICW2 - update interrupt vector offsets
    port_outb(PORT_PIC1_DATA, PIC_OFFSET_MASTER);
    port_wait();
    port_outb(PORT_PIC2_DATA, PIC_OFFSET_SLAVE);
    port_wait();

    // ICW3 - configure cascading between master and slave
    port_outb(PORT_PIC1_DATA, ICW3_IRQ2_HAS_SLAVE);
    port_wait();
    port_outb(PORT_PIC2_DATA, IRQ2_SLAVE);
    port_wait();

    // ICW4 - configure 8086 mode
    port_outb(PORT_PIC1_DATA, ICW4_MODE_8086);
    port_wait();
    port_outb(PORT_PIC2_DATA, ICW4_MODE_8086);
    port_wait();

    // Mask all IRQs on both PICs initially
    port_outb(PORT_PIC1_DATA, 0xFF);
    port_wait();
    port_outb(PORT_PIC2_DATA, 0xFF);
    port_wait();

    // Unmask IRQ0 (timer) and IRQ1 (keyboard) on master PIC
    uint8_t mask_master = port_inb(PORT_PIC1_DATA);
    mask_master &= ~((1 << IRQ0_TIMER) | (1 << IRQ1_KEYBOARD)); // Clear bits 0 and 1
    port_outb(PORT_PIC1_DATA, mask_master);
    port_wait();

    // Keep slave PIC IRQs masked (you can change this if needed)
    uint8_t mask_slave = port_inb(PORT_PIC2_DATA);
    port_outb(PORT_PIC2_DATA, mask_slave);
    port_wait();

    // Clear any outstanding interrupts
    pic_eoi_master();
    pic_eoi_slave();
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PORT_PIC1_DATA;
    } else {
        port = PORT_PIC2_DATA;
        irq -= 8;
    }

    value = port_inb(port);
    value &= ~(1 << irq);
    port_outb(port, value);
}
