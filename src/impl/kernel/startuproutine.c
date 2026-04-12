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
#include "time.h"
#include "x86_64/ac97_driver.h"
#include "x86_64/exec_trace.h"

#define HEAP_MIN_START 0x400000  // 4 MiB - fallback if kernel end is lower

static inline uintptr_t align_up_uintptr(uintptr_t value, uintptr_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

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
    extern char _kernel_end;
    uintptr_t heap_start = align_up_uintptr((uintptr_t)&_kernel_end, 0x1000);
    if (heap_start < HEAP_MIN_START) {
        heap_start = HEAP_MIN_START;
    }
    if (heap_start >= total_ram_bytes) {
        PANIC("Heap start beyond total RAM");
    }
    uint64_t uintheapsize = total_ram_bytes - heap_start;
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str("Startup Routine\n");
    print_str("init rtc\n");
    rtc_init();
    print_str("Time subsystem init\n");
    time_init();
    print_str("init pit: ");
    pit_init(100);
    print_str("Setting up kernel ints\n");
    setup_kernel_interrupts();
    print_str("Setting IDT handler\n");
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

    cli();
    extern uint64_t pit_get_ticks();
    print_uint64_dec(pit_get_ticks()); // Should be 0
    sti();
    while (pit_get_ticks() < 5) {
        asm volatile("hlt");
    }
    serial_write_str("Heap start: 0x");
    serial_write_hex(heap_start);
    serial_write_str("\n");
    allocator_init((void*)heap_start, uintheapsize);
    pmm_init((void*)0x1000000, 0x4000000);  // Start at 16MB instead
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
