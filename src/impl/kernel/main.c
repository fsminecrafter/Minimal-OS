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
#include "serial.h"

#include "x86_64/ac97_driver.h"
#include "audio.h"

#include "keyboard/unifiedkeyboardbridge.h"

//Applications

#include "applications/terminal.h"
#include "keyboard/swedishKeyboard.h"
#include "keyboard/usKeyboard.h"

//MinimaFS

#include "x86_64/minimafs.h"
#include "x86_64/ahci.h"

#include "x86_64/exec_trace.h"

//Audio

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

    if (usb_init()) {
        serial_write_str("USB keyboard available!\n");
    } else {
        serial_write_str("Falling back to PS/2\n");
    }

    /*
     * NOTE: terminal_init_keyboard() used to be called here AND again
     * inside terminal_program_entry() a little further down. That
     * double call was redundant (harmless, but wasteful - it re-runs
     * usb_keyboard_init()/layout load/callback registration and
     * re-logs USB-vs-PS/2 detection a second time) so it has been
     * removed from here; terminal_program_entry() is the single place
     * that now initializes the keyboard, right before the terminal
     * actually needs it.
     *
     * This was NOT the cause of the intermittent PS/2 fallback -
     * terminal_init_keyboard() never touches the USB host controller's
     * enumeration state, only this driver's own HID-report processing
     * state. The real cause was sleep() silently no-op'ing during
     * usb_init()'s enumeration because no process exists yet at this
     * point in boot - see the fix and comment in scheduler.c's
     * sleep().
     */

    initializeGraphicsDevice();
    const char* proc_list[32];
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

    audio_init();

    // Initialize hardware driver (AC97)
    if (!ac97_init()) {
        serial_write_str("ERROR: Audio hardware not found!\n");
        return;
    }
    sti();
    createProcess("busy", busy);
    createProcess("kernelaudio", audioupdate);
    terminal_program_entry();
    while(1);
}
