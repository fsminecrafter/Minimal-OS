#pragma once

#include <stdint.h>

extern uint8_t kernel_stack_top[];

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];       // IST1..IST7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} tss_entry_t;

extern tss_entry_t tss;