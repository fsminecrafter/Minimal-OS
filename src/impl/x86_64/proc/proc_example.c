#include <stdint.h>
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "time.h"
#include "serial.h"
#include "panic.h"

// Simple test process entry point
void test_process_entry(void) {
    serial_write_str("\n\n");
    serial_write_str("========================================\n");
    serial_write_str("   PROCESS ENTRY POINT TEST\n");
    serial_write_str("========================================\n");
    serial_write_str("SUCCESS! The process started!\n");
    
    if (current_process) {
        serial_write_str("Process name: ");
        serial_write_str(current_process->name);
        serial_write_str("\n");
    }
    serial_write_str("========================================\n\n");
    
    int counter = 0;
    while (1) {
        serial_write_str("[TEST] Counter = ");
        serial_write_dec(counter++);
        serial_write_str("\n");
        sleep(500);
    }
}

// Process that prints a message every second
void heartbeat_process(void) {
    int counter = 0;
    
    serial_write_str("\n[HEARTBEAT] Process started!\n");
    
    while (1) {
        counter++;
        serial_write_str("[HEARTBEAT] Alive, counter = ");
        serial_write_dec(counter);
        serial_write_str("\n");
        
        sleep(1000);
    }
}

// Process that counts rapidly then sleeps
void counter_process(void) {
    int iteration = 0;
    
    serial_write_str("\n[COUNTER] Process started!\n");
    
    while (1) {
        iteration++;
        
        serial_write_str("[COUNTER] Iteration ");
        serial_write_dec(iteration);
        serial_write_str(" - Counting...\n");
        
        volatile int count = 0;
        for (int i = 0; i < 1000000; i++) {
            count++;
        }
        
        serial_write_str("[COUNTER] Done, sleeping\n");
        sleep(2000);
    }
}

// Process that displays system info
void sysinfo_process(void) {
    serial_write_str("\n[SYSINFO] Process started!\n");
    
    while (1) {
        serial_write_str("\n=== System Information ===\n");
        
        char uptime_str[32];
        time_format_uptime(uptime_str, sizeof(uptime_str));
        serial_write_str("Uptime: ");
        serial_write_str(uptime_str);
        serial_write_str("\n");
        
        scheduler_print_stats();
        serial_write_str("==========================\n\n");
        
        sleep(5000);
    }
}

// Quick blinker process
void blinker_process(void) {
    int state = 0;
    
    serial_write_str("\n[BLINKER] Process started!\n");
    
    while (1) {
        serial_write_str(state ? "*" : ".");
        state = !state;
        sleep(500);
    }
}

// Main test - simple single process
void proc_test_sleep(void) {
    serial_write_str("\n\n");
    serial_write_str("############################################\n");
    serial_write_str("#  STARTING PROCESS TEST\n");
    serial_write_str("############################################\n\n");
    
    serial_write_str("Creating test process...\n");
    
    process_t* proc_test = proc_create("test", test_process_entry);
    if (!proc_test) {
        PANIC("Cannot start test process");
        return;
    }
    
    serial_write_str("Test process created: ");
    serial_write_str(proc_test->name);
    serial_write_str("\n");
    
    // Set as current and running
    current_process = proc_test;
    current_process->state = PROCESS_RUNNING;
    
    serial_write_str("Jumping to process...\n\n");
    
    // Jump to the process manually (correct assembly syntax)
    __asm__ volatile(
        "mov %0, %%rsp\n\t"       // Set stack pointer
        "push %1\n\t"              // Push RFLAGS
        "popfq\n\t"                // Restore RFLAGS  
        "jmp *%2\n\t"              // Jump to entry point
        : /* no outputs */
        : "r"(proc_test->regs[6]),  // RSP
          "r"(proc_test->regs[8]),  // RFLAGS
          "r"(proc_test->regs[7])   // RIP
        : "memory"
    );
    
    // Should never reach here
    PANIC("Returned from process!");
}

// Alternative: Multiple processes with scheduler
void proc_test_all(void) {
    serial_write_str("\n\n");
    serial_write_str("############################################\n");
    serial_write_str("#  STARTING MULTI-PROCESS TEST\n");
    serial_write_str("############################################\n\n");
    
    // Create all processes
    process_t* proc_heartbeat = proc_create("heartbeat", heartbeat_process);
    if (!proc_heartbeat) {
        PANIC("Cannot start heartbeat");
        return;
    }
    
    process_t* proc_counter = proc_create("counter", counter_process);
    if (!proc_counter) {
        PANIC("Cannot start counter");
        return;
    }
    
    process_t* proc_sysinfo = proc_create("sysinfo", sysinfo_process);
    if (!proc_sysinfo) {
        PANIC("Cannot start sysinfo");
        return;
    }
    
    process_t* proc_blinker = proc_create("blinker", blinker_process);
    if (!proc_blinker) {
        PANIC("Cannot start blinker");
        return;
    }
    
    serial_write_str("All processes created:\n");
    serial_write_str("  - heartbeat\n");
    serial_write_str("  - counter\n");
    serial_write_str("  - sysinfo\n");
    serial_write_str("  - blinker\n\n");
    
    // Set first as running
    current_process = proc_heartbeat;
    current_process->state = PROCESS_RUNNING;
    
    // Others ready
    proc_counter->state = PROCESS_READY;
    proc_sysinfo->state = PROCESS_READY;
    proc_blinker->state = PROCESS_READY;
    
    serial_write_str("Starting scheduler...\n\n");
    schedulerInit();
    
    // Jump to first process
    serial_write_str("Jumping to heartbeat process...\n\n");

}