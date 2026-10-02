#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    union {
        struct {
            uint8_t red_position;
            uint8_t red_mask_size;
            uint8_t green_position;
            uint8_t green_mask_size;
            uint8_t blue_position;
            uint8_t blue_mask_size;
        } rgb;
        struct {
            uint32_t palette_address;
            uint16_t palette_colors;
        } __attribute__((packed)) indexed;
    } color_info;
} multiboot2_framebuffer_tag_t;

typedef struct {
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
} multiboot2_framebuffer_info_t;

uint64_t get_total_memory(multiboot2_info_t* mb_info);
bool multiboot2_get_framebuffer(multiboot2_info_t* mb_info,
                                multiboot2_framebuffer_info_t* framebuffer);
bool multiboot2_has_cmdline_token(multiboot2_info_t* mb_info,
                                  const char* token);