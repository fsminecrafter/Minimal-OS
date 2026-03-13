#include "print.h"
#include "x86_64/rtc.h"
#include "x86_64/commandhandler.h"
#include "x86_64/multiboot2parse.h"
#include "x86_64/startuproutine.h"
#include "x86_64/allocator.h"
#include "proc_example.h"
#include "x86_64/proc.h"
#include "panic.h"
#include "keyboard.h"
#include "keyboardhandler.h"
#include "x86_64/scheduler.h"
#include "x86_64/pci.h"
#include "x86_64/gpu.h"
#include "time.h"
#include "x86_64/memoryscanner.h"
#include "string.h"
#include "prochandler.h"
#include "usb/uhci.h"
#include "x86_64/globaldatatable.h"

#include "keyboard/unifiedkeyboardbridge.h"

//Applications

#include "applications/terminal.h"
#include "keyboard/swedishKeyboard.h"
#include "keyboard/usKeyboard.h"

void busy(void) {
    for (volatile int i = 0; i < 100; i++);
}

void kernel_main(uint64_t mb2_info_addr) {
    multiboot2_info_t* mb_info = (multiboot2_info_t*)mb2_info_addr;
    uint64_t total_ram_bytes = get_total_memory(mb_info);
    print_clear();
    commandhandler_init();
    print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
    print_str("Minimal OS\n");
    print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
    print_uint64_dec(total_ram_bytes / 1024);
    print_str("KiB / ");
    print_uint64_dec(total_ram_bytes / 1048576);
    print_str("MiB");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str("\n");
    //init allocator only beyond - 0x300000
    //allocator_init((void*)0x300000, 1024);
    startroutine(total_ram_bytes);
    //verify_memory_initialization((void*)0x400000, (void*)total_ram_bytes);

    //memory_scanner_init();
    //memory_scan_full();

    pci_enumerate_all();
    
    if (usb_init()) {
        serial_write_str("USB keyboard available!\n");
    } else {
        serial_write_str("Falling back to PS/2\n");
    }
    terminal_init_keyboard();

    initializeGraphicsDevice();
    char *proc_list[32];
    getprocslistNames(proc_list, 32);
    serial_write_str(proc_list[0]);

    datetime_t dt;

    dt.year = 2025;
    dt.month = 5;
    dt.day = 7;
    dt.hour = 15;
    dt.minute = 45;
    dt.second = 30;

    time_set_datetime(&dt);
    terminal_program_entry();
    createProcess("busy", busy);
    schedulerInit();

    while(1);
}