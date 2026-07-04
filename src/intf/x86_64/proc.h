#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <stddef.h>

#define MAX_PROCESS_NAME_LEN 128
#define STACK_SIZE 0x1000  // 4 KB

// Process states
typedef enum {
    PROCESS_READY = 0,      // Ready to run
    PROCESS_RUNNING,        // Currently executing
    PROCESS_WAITING,        // Waiting (sleeping or blocked)
    PROCESS_PAUSED,         // Paused (won't be scheduled)
    PROCESS_ZOMBIE,         // Finished but not cleaned up
    PROCESS_TERMINATED      // Fully terminated
} process_state_t;

// Process control block
typedef struct process {
    uint64_t pid;                      // Process ID
    char name[MAX_PROCESS_NAME_LEN];   // Process name
    process_state_t state;             // Current state

    // Register state (for context switching)
    uint64_t regs[9];                  // RBX, RBP, R12-R15, RSP, RIP, RFLAGS

    // Memory management
    uint64_t pml4;                     // Page table (physical address)
    uint64_t* kernel_stack;            // Kernel stack pointer

    // Timing
    uint64_t wake_time_ms;             // When to wake if sleeping (0 = not sleeping)
    uint64_t cpu_time_ms;              // Total CPU time used
    uint64_t creation_time_ms;         // When process was created

    // Linked list
    struct process* next;              // Next process in list

    // Real process entry point. regs[7] (the saved RIP) is ALWAYS set
    // to the internal trampoline (proc_trampoline in proc.c), never to
    // this pointer directly - see proc_trampoline()'s comment for why.
    //
    // IMPORTANT: this field is appended at the very END of the struct
    // on purpose. context_switch.asm indexes into this struct using
    // hard-coded byte offsets (regs at 0x90, pml4 at 0xD8, kernel_stack
    // at 0xE0). No field before `next` may ever move, grow, or shrink
    // without updating that assembly to match. Adding a field after
    // all of them is safe.
    void (*entry_point)();
} process_t;

// Global process list head (defined in proc.c)
extern process_t* proc_list_head;

// Process management functions
process_t* proc_create(const char* file_name, void (*entry_point)());
void kill(process_t* proc);
process_t* get_proc_by_name(const char* name);
process_t** get_procs(size_t* count);

// Process ID management
uint64_t get_next_pid(void);

// Kernel page table
uint64_t get_kernel_pml4(void);

// Helper functions (from string.h but needed here)
void hex_to_str(uint64_t value, char* out);
void uint_to_str(uint64_t value, char* out);

#endif // PROC_H
