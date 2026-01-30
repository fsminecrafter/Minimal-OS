#include <stdint.h>
#include <stddef.h>
#include "x86_64/proc.h"
#include "panic.h"
#include "print.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "serial.h"

// The currently running process
process_t* current_process = NULL;

// Linked list of processes
extern process_t* proc_list_head;

// Forward declaration of context switch assembly routine
extern void context_switch(process_t* current, process_t* next);

// Call to yield current process (mark ready)
void ready() {
    if (current_process)
        current_process->state = PROCESS_READY;
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
        default: {
            char state_str[21];
            uint_to_str((uint64_t)current_process->state, state_str);
            PANIC(state_str);
        }
    }
}

// Main scheduling function
void schedule() {
    // Clean up ZOMBIE or TERMINATED processes
    process_t* prev = NULL;
    process_t* curr = proc_list_head;

    while (curr) {
        if (curr->state == PROCESS_ZOMBIE || curr->state == PROCESS_TERMINATED) {
            process_t* to_free = curr;
            if (prev) prev->next = curr->next;
            else proc_list_head = curr->next;

            void* stack_base = (void*)((uint8_t*)to_free->kernel_stack - STACK_SIZE);
            free_mem(stack_base);
            free_mem(to_free);

            curr = (prev) ? prev->next : proc_list_head;
            continue;
        }
        prev = curr;
        curr = curr->next;
    }

    if (!current_process) {
        // First process to run
        current_process = proc_list_head;
        if (!current_process)
            PANIC("No processes to schedule");
        current_process->state = PROCESS_RUNNING;
        return;
    }

    // Round-robin to next READY process
    process_t* next = current_process->next ? current_process->next : proc_list_head;

    process_t* start = next;
    while (next && next->state != PROCESS_READY) {
        next = next->next ? next->next : proc_list_head;
        if (next == start) {
            // No other READY process found
            if (current_process->state == PROCESS_RUNNING || current_process->state == PROCESS_READY)
                return;
            else
                PANIC("No runnable processes found");
        }
    }

    if (!next || next == current_process)
        return;

    // Switch
    process_t* old = current_process;
    old->state = PROCESS_READY;
    next->state = PROCESS_RUNNING;
    current_process = next;

    serial_write_str("Switching...\n");
    context_switch(old, next);
}

static int tick = 0;

// Timer-based scheduler trigger (called from PIT)
void scheduler_tick() {
    if (!current_process) return;

    tick++;
    if (tick >= 10) {
        tick = 0;

        process_t* last = current_process;
        schedule();

        if (last != current_process) {
            serial_write_str("Scheduling...\n");
            serial_write_str(current_process->name);
            serial_write_str("\n");
        }
    }
}
