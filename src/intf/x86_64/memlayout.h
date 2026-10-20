#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/multiboot2parse.h"

typedef struct {
    uint64_t start;     // physical (= virtual, identity mapped)
    uint64_t end;
    char cmdline[128];
} memlayout_module_t;

// Copies the multiboot2 info into the kernel image so the heap can never
// overwrite it. Returns the copy (or mb itself if it is too big to copy).
multiboot2_info_t* memlayout_save_multiboot(multiboot2_info_t* mb);

// Biggest free block of available RAM below 4 GiB that does not overlap
// the kernel image or any boot module.
bool memlayout_pick_heap_region(multiboot2_info_t* mb, uintptr_t kernel_end,
                                uint64_t* start, uint64_t* end);

// Marks identity-mapped 2 MiB pages without RAM as uncached.
void memlayout_apply_cache_policy(multiboot2_info_t* mb);

// GRUB "module2 <file> <name>" lookups.
bool memlayout_find_module(multiboot2_info_t* mb, const char* name,
                           memlayout_module_t* out);
bool memlayout_get_module(multiboot2_info_t* mb, uint32_t index,
                          memlayout_module_t* out);

// The multiboot info kernel_main is using (the saved copy).
extern multiboot2_info_t* kernel_multiboot_info;
