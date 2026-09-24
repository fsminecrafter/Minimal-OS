#include "print.h"
#include "x86_64/commandhandler.h"
#include "x86_64/multiboot2parse.h"
#include "x86_64/startuproutine.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/idt.h"
#include "x86_64/gdt.h"
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
#include "x86_64/smp.h"

#include "x86_64/network_manager.h"
#include "x86_64/rtl8139.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/ip.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "x86_64/net_syscall.h"
#include "x86_64/random.h"


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
    serial_init();
    smp_init_bsp();
    gdt_init();
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

    network_manager_register_driver(rtl8139_get_driver());
    if (network_manager_init()) {
        eth_init();
        arp_init();
        ip_init();
        udp_init();
        tcp_init();
        // Handle table + per-port UDP receive queues that SYS_NET
        // hands to user processes (see net_syscall.c).
        net_syscall_init();
        uint8_t mac[6];
        network_manager_get_mac(mac);
        if (mac[0] == 0x52 && mac[1] == 0x54 && mac[2] == 0x00 &&
            mac[3] == 0x12 && mac[4] == 0x34 &&
            (mac[5] == 0x56 || mac[5] == 0x57)) {
            uint32_t static_ip = (mac[5] == 0x56) ? 0xC0A86402u : 0xC0A86403u;
            ip_configure(static_ip, 0xFFFFFF00u, 0);
            serial_write_str("Network: run-two static LAN address configured\n");
        } else {
            serial_write_str("Network: interface ready (run 'dhcp' to get an address)\n");
        }
    } else {
        serial_write_str("Network: no supported NIC found\n");
    }

    time_set_from_str_if_newer(BUILD_DATE);

    // Seeded after the clock is set so the RTC contributes something
    // real. Userland reaches this through SYS_RANDOM.
    random_init();

    // Registers every built-in audio_hw_driver_t with audio_manager
    // and initializes whichever one is actually present. See
    // audio_manager.h / audio_hw.h for the module interface.
    audio_init();

    // Register AHCI storage driver. The terminal's initdisk command owns
    // controller probing and MinimaFS device creation, so do not initialize
    // AHCI a second time here before the mount prompt.
    storage_manager_register_driver(ahci_get_storage_driver());

    smp_start_aps(mb_info);

    sti();
    createProcess("busy", busy);
    createProcess("kernelaudio", audioupdate);
    terminal_program_entry();
    while(1);
}
