#include "print.h"
#include "x86_64/commandhandler.h"
#include "x86_64/multiboot2parse.h"
#include "x86_64/startuproutine.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/idt.h"
#include "x86_64/gpu.h"
#include "x86_64/gpu_manager.h"
#include "time.h"
#include "prochandler.h"
#include "usb/uhci.h"
#include "x86_64/usb_manager.h"
#include "x86_64/globaldatatable.h"
#include "x86_64/exec_trace.h"
#include "serial.h"

#include "audio.h"
#include "keyboard/usbkeyboard.h"
#include "keyboard/swedishKeyboard.h"
#include "keyboard/usKeyboard.h"
#include "applications/terminal.h"
#include "x86_64/ahci.h"
#include "x86_64/storage_manager.h"
#include "x86_64/global_audio_state.h"

void busy(void) {
    serial_write_str("Busy process\n");
    process_exit();
}

void audioupdate(void) {
    while (1) {
        if (g_audio_state.playing && g_audio_state.player) {
            audio_player_update(g_audio_state.player);
        }
        sleep(2);
    }
}

/*
 * usb_keyboard_update() (declared in keyboard/usbkeyboard.h) is a
 * single-pass function meant to be invoked periodically - it checks
 * whether the currently-held key has been down long enough to fire a
 * repeat, then returns. It has no internal loop.
 *
 * It must NOT be used directly as a process entry point: every process
 * entry function is expected to either loop forever or terminate
 * itself via process_exit()/kill(). A function that simply returns
 * once is now caught by proc_trampoline()'s safety net (so it can no
 * longer corrupt/hang the system), but running it only once still
 * means USB key-repeat handling would silently stop working after the
 * very first tick. This wrapper gives it the periodic loop it actually
 * needs.
 */
void usb_keyboard_update_task(void) {
    while (1) {
        usb_keyboard_update();
        sleep(10);
    }
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
    print_str("KiB ||  ");
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

    addvar(total_ram_bytes, "totalrambytes");

    pci_enumerate_all();

    // Registers every built-in usb_hw_driver_t with usb_manager and
    // brings up whichever host controller is actually present. See
    // usb_manager.h / usb_hw.h for the module interface.
    usb_manager_register_driver(uhci_get_driver());
    if (usb_manager_init()) {
        serial_write_str("USB keyboard available!\n");
    } else {
        serial_write_str("Falling back to PS/2\n");
    }

    // Registers every built-in gpu_hw_driver_t with gpu_manager and
    // initializes whichever one is actually present. See
    // gpu_manager.h / gpu_hw.h for the module interface.
    gpu_manager_register_driver(gpu_bochs_vbe_get_driver());
    if (!gpu_manager_init()) {
        serial_write_str("No display driver available - continuing headless\n");
    }
    const char* proc_list[32];
    getprocslistNames(proc_list, 32);
    serial_write_str(proc_list[0]);

    datetime_t dt;

    dt.year = 2026;
    dt.month = 8;
    dt.day = 19;
    dt.hour = 18;
    dt.minute = 50;
    dt.second = 30;

    time_set_datetime(&dt);

    // Registers every built-in audio_hw_driver_t with audio_manager
    // and initializes whichever one is actually present. See
    // audio_manager.h / audio_hw.h for the module interface.
    audio_init();

    // Register AHCI storage driver and initialize storage manager
    storage_manager_register_driver(ahci_get_storage_driver());
    if (storage_manager_init()) {
        serial_write_str("Storage manager initialized\n");
    } else {
        serial_write_str("No storage driver found\n");
    }

    sti();
    createProcess("busy", busy);
    createProcess("kernelaudio", audioupdate);
    terminal_program_entry();
    while(1);
}
