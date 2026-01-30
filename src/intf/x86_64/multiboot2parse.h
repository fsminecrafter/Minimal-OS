#pragma once

#include <stdint.h>
#include <stddef.h>

// Multiboot2 info header struct
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
    uint8_t  tags[];
} multiboot2_info_t;

// Generic Multiboot2 tag header
typedef struct {
    uint32_t type;
    uint32_t size;
} multiboot2_tag_t;

// Memory map tag (type 6)
typedef struct {
    uint32_t type;           // == 6 for memory map
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    // Followed by entries[]
} multiboot2_tag_mmap_t;

// Memory map entry struct
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} multiboot2_mmap_entry_t;

uint64_t get_total_memory(multiboot2_info_t* mb_info);