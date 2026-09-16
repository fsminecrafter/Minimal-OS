// src/intf/x86_64/proc.h
#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_PROCESS_NAME_LEN 128
#define STACK_SIZE 0x10000  // 64 KB

// Process states
typedef enum {
    PROCESS_READY = 0,      // Ready to run
    PROCESS_RUNNING,        // Currently executing
    PROCESS_WAITING,        // Waiting (sleeping or blocked)
    PROCESS_PAUSED,         // Paused (won't be scheduled)
    PROCESS_ZOMBIE,         // Finished but not cleaned up
    PROCESS_TERMINATED      // Fully terminated
} process_state_t;

// ===========================================
// PROCESS PRIVILEGE / USER LEVEL
// ===========================================
//
// Classification of *who* a process belongs to - the kernel itself
// (drivers, the terminal, the scheduler's own housekeeping processes)
// versus a user-loaded .run program (see elfloader.c / runcommand.c).
//
// User processes enter through CPL3 with a private PML4 root, user GDT
// segments, a TSS kernel stack, and an iretq transition. Their ELF image
// and stack use process-owned physical pages and are reclaimed on exit.
//
// This classification gives the scheduler, process listings, and syscall
// policy a stable place to ask "is this a program someone ran, or part of the
// kernel itself" - e.g. to eventually restrict which syscalls a user
// process may issue - without guessing from the process name string.
typedef enum {
    PROC_PRIVILEGE_KERNEL = 0,   // Kernel-owned process (drivers, terminal, etc)
    PROC_PRIVILEGE_USER   = 1,   // A loaded .run program (see runcommand.c)
} process_privilege_t;

// Process control block
typedef struct process {
    uint64_t pid;                      // Process ID
    char name[MAX_PROCESS_NAME_LEN];   // Process name
    process_state_t state;             // Current state

    // Register state (for context switching)
    uint64_t regs[9];                  // RBX, RBP, R12-R15, RSP, RIP, RFLAGS

    // Memory management
    uint64_t pml4;                     // Per-process PML4 root (physical address)
    uint64_t* kernel_stack;            // Kernel stack pointer used for CPL0 context
    uint64_t* user_stack;              // User stack pointer for future ring3 entry path

    // Timing
    uint64_t wake_time_ms;             // When to wake if sleeping (0 = not sleeping)
    uint64_t cpu_time_ms;              // Total CPU time used
    uint64_t creation_time_ms;         // When process was created

    // Linked list
    struct process* next;              // Next process in list

    // Entry point used by the process trampoline. For user processes this
    // is the actual user image entry and is entered through iretq.
    //
    // IMPORTANT: this field is appended at the very END of the struct
    // on purpose. context_switch.asm indexes into this struct using
    // hard-coded byte offsets (regs at 0x90, pml4 at 0xD8, kernel_stack
    // at 0xE0). No field before `next` may ever move, grow, or shrink
    // without updating that assembly to match. Adding a field after
    // all of them is safe.
    void (*entry_point)();

    // ===========================================
    // PER-CPU MLFQ SCHEDULER FIELDS
    // ===========================================
    // Appended here for the same reason entry_point is last: nothing
    // above this line may move without touching context_switch.asm.
    // These are only ever read/written by scheduler.c (and its
    // dequeue/enqueue helpers called from proc.c / prochandler.c) -
    // see scheduler.h.
    uint32_t sched_level;        // MLFQ level: SCHED_LEVEL_INTERACTIVE..BACKGROUND
    uint32_t sched_cpu;          // Which per-CPU run queue this process belongs to
    uint32_t sched_ticks_used;   // Ticks consumed in the current dispatch, for demotion
    struct process* rq_next;     // Intrusive next pointer for the per-CPU ready queue.
                                  // Deliberately separate from `next` above, which
                                  // remains the global creation/cleanup list link.

    // Privilege classification - see process_privilege_t above. Safe
    // to append here for the same struct-layout reason as every other
    // field below `next`: context_switch.asm never touches anything
    // past kernel_stack, so this struct can keep growing after it
    // without any assembly changes.
    process_privilege_t privilege;

    // Physical storage owned by a user process. These fields are appended
    // after the context-switch ABI fields and are reclaimed by the scheduler.
    void* user_image_phys;
    size_t user_image_pages;
    void* user_stack_phys;
    size_t user_stack_pages;
    // argv/argc for a user process's entry point, consumed by
    // proc_enter_ring3() via proc_trampoline(). Set by run_launch_file()
    // (runcommand.c) before the process is first scheduled. Safe to
    // append here for the same struct-layout reason as every other
    // field below `next` - context_switch.asm never touches anything
    // past kernel_stack, so this struct can keep growing without any
    // assembly changes.
    uint64_t user_argc;
    char** user_argv;

    // Cooperative cleanup requested by the terminal before forced kill.
    void (*cleanup_entry)();
    uint64_t cleanup_deadline_ms;
    bool cleanup_requested;
    bool cleanup_invoked;
} process_t;

// Global process list head (defined in proc.c)
extern process_t* proc_list_head;

// Process management functions

// Creates a new PROC_PRIVILEGE_KERNEL process. Equivalent to
// proc_create_ex(file_name, entry_point, PROC_PRIVILEGE_KERNEL) - kept
// as its own entry point so every existing caller (there are many)
// keeps compiling and behaving exactly as before.
process_t* proc_create(const char* file_name, void (*entry_point)());

// Full form of proc_create() that also lets the caller set the new
// process's privilege classification up front. See process_privilege_t
// above for what this does (and does not yet) mean.
process_t* proc_create_ex(const char* file_name, void (*entry_point)(),
                          process_privilege_t privilege);

void kill(process_t* proc);
process_t* get_proc_by_name(const char* name);
process_t** get_procs(size_t* count);

// Process ID management
uint64_t get_next_pid(void);

// Kernel page table
uint64_t get_kernel_pml4(void);

// Copy a user range to process-owned physical pages and map only those pages
// as user-accessible in the process's private page-table hierarchy.
int proc_map_user_range(process_t* proc, void* address, size_t length);
void proc_destroy_address_space(process_t* proc);

// Helper functions (from string.h but needed here)
void hex_to_str(uint64_t value, char* out);
void uint_to_str(uint64_t value, char* out);

#endif // PROC_H