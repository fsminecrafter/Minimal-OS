#include <stdint.h>
#include <stddef.h>
#include "x86_64/proc.h"
#include "time.h"
#include "panic.h"
#include "print.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "bool.h"

bool scheduler_on = false;

// The currently running process
process_t* current_process = NULL;

// Linked list of processes
extern process_t* proc_list_head;

// Forward declaration of context switch assembly routine
extern void context_switch(process_t* current, process_t* next);

// Statistics
static uint64_t total_context_switches = 0;
static uint64_t idle_cycles = 0;

// Call to yield current process (mark ready)
void ready() {
    if (current_process)
        current_process->state = PROCESS_READY;
}

void schedulerInit() {
    scheduler_on = true;
}

void sleep(uint64_t milliseconds) {
    if (!current_process) return;
    
    uint64_t wake_time = time_get_uptime_ms() + milliseconds;
    current_process->wake_time_ms = wake_time;
    current_process->state = PROCESS_WAITING;
    
    serial_write_str("Process ");
    serial_write_str(current_process->name);
    serial_write_str(" sleeping for ");
    serial_write_dec(milliseconds);
    serial_write_str("ms\n");
    
    // Mark as waiting and switch to another process
    schedule();
    
    // When we return here, this process has been woken up and rescheduled
    // The state was changed to READY/RUNNING by wake_sleeping_processes()
    // Just return - we're done sleeping!
    
    serial_write_str("Process ");
    serial_write_str(current_process->name);
    serial_write_str(" woke up\n");
}
// Wake up sleeping processes whose time has come
static void wake_sleeping_processes() {
    uint64_t current_time = time_get_uptime_ms();
    
    for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
        if (proc->state == PROCESS_WAITING && proc->wake_time_ms > 0) {
            if (current_time >= proc->wake_time_ms) {
                proc->state = PROCESS_READY;
                proc->wake_time_ms = 0;
                
                serial_write_str("Waking process: ");
                serial_write_str(proc->name);
                serial_write_str("\n");
            }
        }
    }
}

// Print current process state (for debugging)
void currentstate() {
    if (!current_process) {
        print_str("No process\n");
        return;
    }

    switch (current_process->state) {
        case PROCESS_WAITING:    print_char('W'); break;
        case PROCESS_TERMINATED: print_char('T'); break;
        case PROCESS_RUNNING:    print_char('R'); break;
        case PROCESS_READY:      print_str("RR"); break;
        case PROCESS_ZOMBIE:     print_char('Z'); break;
        default: {
            char state_str[21];
            uint_to_str((uint64_t)current_process->state, state_str);
            PANIC(state_str);
        }
    }
}

// Find next ready process using round-robin
static process_t* find_next_ready_process() {
    if (!proc_list_head) return NULL;
    
    // Start from next process after current, or from head if no current
    process_t* start = current_process ? 
        (current_process->next ? current_process->next : proc_list_head) : 
        proc_list_head;
    
    process_t* next = start;
    
    // Look for a READY process
    do {
        if (next->state == PROCESS_READY) {
            return next;
        }
        next = next->next ? next->next : proc_list_head;
    } while (next != start);
    
    // If current process is still runnable, keep it
    if (current_process && 
        (current_process->state == PROCESS_RUNNING || 
         current_process->state == PROCESS_READY)) {
        return current_process;
    }
    
    return NULL;
}

// Main scheduling function
void schedule() {
    
    // Clean up ZOMBIE or TERMINATED processes
    process_t* prev = NULL;
    process_t* curr = proc_list_head;

    while (curr) {
        if (curr->state == PROCESS_ZOMBIE || curr->state == PROCESS_TERMINATED) {
            serial_write_str("Cleaning up process: ");
            serial_write_str(curr->name);
            serial_write_str("\n");
            
            process_t* to_free = curr;
            
            // Remove from list
            if (prev) {
                prev->next = curr->next;
            } else {
                proc_list_head = curr->next;
            }

            // Free resources
            if (to_free->kernel_stack) {
                void* stack_base = (void*)((uint8_t*)to_free->kernel_stack - STACK_SIZE);
                free_mem(stack_base);
            }
            free_mem(to_free);

            curr = (prev) ? prev->next : proc_list_head;
            continue;
        }
        prev = curr;
        curr = curr->next;
    }

    if (!current_process) {
        // First process to run
        current_process = find_next_ready_process();
        if (!current_process) {
            serial_write_str("No processes to schedule\n");
            PANIC("No processes to schedule, but scheduler called");
        }
        current_process->state = PROCESS_RUNNING;
        
        serial_write_str("Starting first process: ");
        serial_write_str(current_process->name);
        serial_write_str("\n");
        return;
    }

    // Find next ready process
    process_t* next = find_next_ready_process();

    if (!next) {
        // No runnable processes - idle
        serial_write_str("No runnable processes - idle\n");
        idle_cycles++;
        return;
    }

    if (next == current_process) {
        // Same process continues running
        return;
    }

    // Perform context switch
    process_t* old = current_process;
    
    // Update states
    if (old->state == PROCESS_RUNNING) {
        old->state = PROCESS_READY;
    }
    next->state = PROCESS_RUNNING;
    
    current_process = next;
    total_context_switches++;

    serial_write_str("Context switch ");
    serial_write_dec(total_context_switches);
    serial_write_str(": ");
    serial_write_str(old->name);
    serial_write_str(" -> ");
    serial_write_str(next->name);
    serial_write_str("\n");
    
    context_switch(old, next);
}

static int tick_counter = 0;
static const int TICKS_PER_SCHEDULE = 10;  // Schedule every 10 ticks

// Timer-based scheduler trigger (called from PIT)
void scheduler_tick() {
    if (!scheduler_on) {
        return; // Prevent re-entrancy
    }

    wake_sleeping_processes(); 

    if (!current_process) {
        schedule();
        return;
    }

    tick_counter++;
    
    // Schedule every N ticks (time slice)
    if (tick_counter >= TICKS_PER_SCHEDULE) {
        tick_counter = 0;
        
        process_t* last = current_process;
        schedule();
        
        // Only print if we actually switched
        if (last != current_process && current_process) {
            // Already printed in schedule()
        }
    }
}

// Get scheduler statistics
void scheduler_get_stats(uint64_t* switches, uint64_t* idle) {
    if (switches) *switches = total_context_switches;
    if (idle) *idle = idle_cycles;
}

// Print scheduler statistics
void scheduler_print_stats() {
    print_str("=== Scheduler Statistics ===\n");
    print_str("Context switches: ");
    print_uint64_dec(total_context_switches);
    print_str("\nIdle cycles: ");
    print_uint64_dec(idle_cycles);
    print_str("\n");
    
    // Count processes by state
    int ready = 0, running = 0, waiting = 0, zombie = 0;
    for (process_t* p = proc_list_head; p != NULL; p = p->next) {
        switch (p->state) {
            case PROCESS_READY: ready++; break;
            case PROCESS_RUNNING: running++; break;
            case PROCESS_WAITING: waiting++; break;
            case PROCESS_ZOMBIE: zombie++; break;
            default: break;
        }
    }
    
    print_str("Processes - Ready: ");
    print_int(ready);
    print_str(", Running: ");
    print_int(running);
    print_str(", Waiting: ");
    print_int(waiting);
    print_str(", Zombie: ");
    print_int(zombie);
    print_str("\n============================\n");
}