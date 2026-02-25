#include <stdint.h>
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "print.h"
#include "panic.h"
#include "bool.h"
#include "graphics.h"

// Process that prints a message every second
void heartbeat_process() {
    int counter = 0;
    while (1) {
        print_set_color(PRINT_COLOR_LIGHT_CYAN, PRINT_COLOR_BLACK);
        print_str("[");
        time_print_datetime(false);
        print_str("] Heartbeat ");
        print_int(counter++);
        print_str("\n");
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        
        graphics_fill_circle(100 + (counter % 500), 100, 20, 255, 0, 0);

        // Sleep for 1 second (1000ms)
        sleep(1000);
    }
}

// Process that counts rapidly for a bit, then sleeps
void counter_process() {
    int iteration = 0;
    while (1) {
        iteration++;
        
        print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
        print_str("[Counter] Iteration ");
        print_int(iteration);
        print_str(" - Counting to 1000000...\n");
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        
        volatile int count = 0;
        for (int i = 0; i < 1000000; i++) {
            count++;
        }
        
        print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
        print_str("[Counter] Done counting, sleeping for 2 seconds\n");
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        
        // Sleep for 2 seconds
        sleep(2000);
    }
}

// Process that displays system info periodically
void sysinfo_process() {
    while (1) {
        print_set_color(PRINT_COLOR_MAGENTA, PRINT_COLOR_BLACK);
        print_str("\n=== System Information ===\n");
        print_str("Time: ");
        time_print_datetime(true);
        print_str("\n");
        
        char uptime_str[32];
        time_format_uptime(uptime_str, sizeof(uptime_str));
        print_str("Uptime: ");
        print_str(uptime_str);
        print_str("\n");
        
        scheduler_print_stats();
        print_str("==========================\n\n");
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        
        // Sleep for 5 seconds
        sleep(5000);
    }
}

// Quick blinker process
void blinker_process() {
    int state = 0;
    while (1) {
        if (state) {
            print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
            print_str("*");
        } else {
            print_set_color(PRINT_COLOR_BLUE, PRINT_COLOR_BLACK);
            print_str(".");
        }
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        
        state = !state;
        
        // Sleep for 500ms (blink twice per second)
        sleep(500);
    }
}

// Main test function to start all processes
void proc_test_sleep() {
    print_str("Starting sleep test with multiple processes...\n\n");
    
    // Create processes
    process_t* proc_heartbeat = proc_create("heartbeat", heartbeat_process);
    if (!proc_heartbeat) {
        PANIC("Cannot start heartbeat process");
        return;
    }
    
    process_t* proc_counter = proc_create("counter", counter_process);
    if (!proc_counter) {
        PANIC("Cannot start counter process");
        return;
    }
    
    process_t* proc_sysinfo = proc_create("sysinfo", sysinfo_process);
    if (!proc_sysinfo) {
        PANIC("Cannot start sysinfo process");
        return;
    }
    
    process_t* proc_blinker = proc_create("blinker", blinker_process);
    if (!proc_blinker) {
        PANIC("Cannot start blinker process");
        return;
    }
    
    // Set first process as running
    current_process = proc_heartbeat;
    current_process->state = PROCESS_RUNNING;
    
    // Others start ready
    proc_counter->state = PROCESS_READY;
    proc_sysinfo->state = PROCESS_READY;
    proc_blinker->state = PROCESS_READY;
    
    print_str("All processes created. Starting scheduler...\n\n");
    
}