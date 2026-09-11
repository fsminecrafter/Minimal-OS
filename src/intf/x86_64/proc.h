// src/intf/x86_64/proc.h
#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <stddef.h>

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
// This is METADATA ONLY today. Every process, kernel or "user", still
// executes at CPL0 with interrupts and I/O fully available, and every
// process_t still shares the exact same kernel PML4 (see
// proc_create_ex()'s pml4 assignment, and thread.h's comment making
// this same point about threads vs. processes). Real ring3 isolation
// needs three things this kernel does not have yet, each already
// flagged as future work elsewhere in this project:
//
//   1. Per-process address spaces - without this a PROC_PRIVILEGE_USER
//      process can still read/write any kernel memory it wants, so
//      CPL3 alone would be security theater. (This is the same
//      "future change will require revisiting the global FD table
//      design" dependency already tracked for the syscall FD table.)
//   2. User-mode GDT code/data segments - gdt.c only ever builds the
//      DPL0 kernel CS/DS pair (see ACC_* flags in gdt.c), plus a TSS
//      rsp0 to use on privilege transitions.
//   3. A real privilege-transition path - the syscall gate already
//      has DPL3 set (see idt_init()'s 0x80 entry in idt.c), but
//      nothing currently performs an iret that actually drops CPL to
//      3 on entry to a "user" process.
//
// Until all three land, this classification exists purely so the
// scheduler, process listings, and future syscall policy have a
// stable place to ask "is this a program someone ran, or part of the
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

// Helper functions (from string.h but needed here)
void hex_to_str(uint64_t value, char* out);
void uint_to_str(uint64_t value, char* out);

#endif // PROC_H