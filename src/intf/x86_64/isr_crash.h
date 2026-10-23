#pragma once
#include <stdint.h>
#include <stddef.h>

// The last CPU exception that terminated a user process, for the
// terminal's crash message (isr_handlers.c fills it in).
typedef struct {
    uint64_t pid;
    uint8_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t fault_address;     // CR2, page faults only
    uintptr_t image_base;       // where the program's image was loaded
    size_t image_size;
    char name[64];
} user_crash_info_t;

const user_crash_info_t* isr_last_user_crash(void);
