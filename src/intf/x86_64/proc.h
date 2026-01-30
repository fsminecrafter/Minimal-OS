#pragma once
#include <stdint.h>
#include <stddef.h>

#define MAX_PROCESS_NAME_LEN 128
#define STACK_SIZE 0x1000 

typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_WAITING,
    PROCESS_TERMINATED,
    PROCESS_ZOMBIE
} process_state_t;

typedef struct process {
    char          name[MAX_PROCESS_NAME_LEN];      // offsets 0x00–0x7F
    uint64_t      pid;            // 0x80–0x87
    uint64_t*     pml4;           // 0x88–0x8F
    uint64_t*     kernel_stack;   // 0x90–0x97
    uint64_t      rip;            // 0x98–0x9F
    uint64_t      rsp;            // 0xA0–0xA7
    uint64_t      regs[16];       // 0xA8–0xA8+0x80-1 = 0xA8–0x127
    process_state_t state;        // 0x128–0x12B
    struct process* next;         // 0x130–0x137
} process_t;


process_t* proc_create(const char* file_name, void (*entry_point)());
uint64_t* get_kernel_pml4(void);
void kill(process_t* proc);
process_t* get_proc_by_name(const char* name);
process_t** get_procs(size_t* count);