#include "print.h"
#include "keyboard.h"
#include "x86_64/rtc.h"
#include "keyboardhandler.h"
#include "x86_64/pit.h"
#include "x86_64/idt.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "allocator_tester.h"
#include "x86_64/pcaudio.h"
#include "panic.h"
#include "x86_64/port.h"
#include "serial.h"
#include "x86_64/gpu.h"
#include "x86_64/gdt.h"
#include "x86_64/tss.h"

#define HEAP_START 0x300000  // 3 MiB
#define HEAP_SIZE  (total_ram_bytes - HEAP_START)

void print_pic_masks() {
    uint8_t master_mask = port_inb(0x21);
    uint8_t slave_mask = port_inb(0xA1);
    print_str("PIC Master Mask: ");
    print_uint64_hex(master_mask);
    print_str("\n");
    print_str("PIC Slave Mask: ");
    print_uint64_hex(slave_mask);
    print_str("\n");
}

void startroutine(uint64_t total_ram_bytes) {
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str("Startup Routine\n");
    print_str("init pit: ");
    pit_init(1000);
    setup_kernel_interrupts();
    idt_set_handler_pit(pit_irq_handler);
    print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
    if (pit_get_ticks == 0) {
        PANIC("Cannot start/init/get response from pit");
    }
    print_str("[OK]\n");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_pic_masks();
    print_str("Running alloc test\n");
    test();
    //beep();
    print_str("Running pit test\n");

    asm volatile("cli");
    extern uint64_t pit_get_ticks();
    print_uint64_dec(pit_get_ticks()); // Should be 0
    asm volatile("sti");
    while (pit_get_ticks() < 5) {
        asm volatile("hlt");
    }
    allocator_init((void*)HEAP_START, HEAP_SIZE);
    pmm_init((void*)0x100000, 0x4000000);
    print_str("PIT working!\n");
    print_str("Init serial\n");
    serial_init();
    print_str("Init MMIO subsystem\n");
    extern void mmio_init(void);
    mmio_init();
    print_str("Enumerating PCI devices\n");
    pci_enumerate_all();
    print_pci_devices();
    

    print_str("Startup routine Done.\n");
}