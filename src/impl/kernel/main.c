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

    memory_scanner_init();
    memory_scan_full();

    test_gpu();
    proc_test_sleep();
    schedulerInit();

    while(1);
}