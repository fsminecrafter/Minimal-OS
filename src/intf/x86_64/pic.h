#ifndef PIC_H
#define PIC_H

#include <stdint.h>

#define PORT_PIC1_COMMAND 0x20
#define PORT_PIC1_DATA 0x21
#define PORT_PIC2_COMMAND 0xA0
#define PORT_PIC2_DATA 0xA1

#define IRQ0_TIMER 0x00
#define IRQ1_KEYBOARD 0x01
#define IRQ2_SLAVE 0x02

#define ICW1_INIT 0x10
#define ICW1_HAS_ICW4 0x01

#define ICW3_IRQ2_HAS_SLAVE (1 << IRQ2_SLAVE)

#define ICW4_MODE_8086 (1 << 0)

#define PIC_EOI 0x20
#define PIC_OFFSET_MASTER 0x20
#define PIC_OFFSET_SLAVE 0x28

void pic_remap();
void pic_eoi_master();
void pic_eoi_slave();
void pic_unmask_irq(uint8_t irq);


#endif
