#include <stdint.h>
#include <stddef.h>
#include "x86_64/multiboot2parse.h"

// Returns total available RAM in bytes by parsing the multiboot2 memory map
uint64_t get_total_memory(multiboot2_info_t* mb_info) {
    uint64_t total_mem = 0;

    // Pointer to start of tags
    uint8_t* ptr = mb_info->tags;
    // End of the multiboot2 info structure
    uint8_t* end = (uint8_t*)mb_info + mb_info->total_size;

    while (ptr < end) {
        multiboot2_tag_t* tag = (multiboot2_tag_t*)ptr;

        if (tag->type == 0) {
            // End tag
            break;
        }

        if (tag->type == 6) { // Memory map tag
            multiboot2_tag_mmap_t* mmap_tag = (multiboot2_tag_mmap_t*)tag;
            size_t entries_count = (mmap_tag->size - sizeof(multiboot2_tag_mmap_t)) / mmap_tag->entry_size;

            multiboot2_mmap_entry_t* entries = (multiboot2_mmap_entry_t*)(mmap_tag + 1);

            for (size_t i = 0; i < entries_count; i++) {
                // Type 1 indicates available RAM
                if (entries[i].type == 1) {
                    total_mem += entries[i].length;
                }
            }
        }

        // Advance pointer to next tag (rounded up to 8 bytes alignment)
        ptr += (tag->size + 7) & ~7;
    }

    return total_mem;
}
