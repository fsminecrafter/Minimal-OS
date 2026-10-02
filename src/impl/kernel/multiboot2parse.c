#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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

bool multiboot2_get_framebuffer(multiboot2_info_t* mb_info,
                                multiboot2_framebuffer_info_t* framebuffer) {
    if (!mb_info || !framebuffer || mb_info->total_size < sizeof(*mb_info)) {
        return false;
    }

    uintptr_t start = (uintptr_t)mb_info;
    if (mb_info->total_size > UINTPTR_MAX - start) return false;
    uintptr_t end = start + mb_info->total_size;
    uintptr_t cursor = (uintptr_t)mb_info->tags;

    while (cursor <= end && end - cursor >= sizeof(multiboot2_tag_t)) {
        const multiboot2_tag_t* tag = (const multiboot2_tag_t*)cursor;
        if (tag->size < sizeof(*tag) || tag->size > end - cursor) return false;
        if (tag->type == 0) return false;

        if (tag->type == 8) {
            if (tag->size < sizeof(multiboot2_framebuffer_tag_t)) return false;
            const multiboot2_framebuffer_tag_t* fb =
                (const multiboot2_framebuffer_tag_t*)tag;
            if (fb->framebuffer_type != 1) return false;

            framebuffer->address = fb->address;
            framebuffer->pitch = fb->pitch;
            framebuffer->width = fb->width;
            framebuffer->height = fb->height;
            framebuffer->bpp = fb->bpp;
            framebuffer->framebuffer_type = fb->framebuffer_type;
            framebuffer->red_position = fb->color_info.rgb.red_position;
            framebuffer->red_mask_size = fb->color_info.rgb.red_mask_size;
            framebuffer->green_position = fb->color_info.rgb.green_position;
            framebuffer->green_mask_size = fb->color_info.rgb.green_mask_size;
            framebuffer->blue_position = fb->color_info.rgb.blue_position;
            framebuffer->blue_mask_size = fb->color_info.rgb.blue_mask_size;
            return true;
        }

        uintptr_t advance = ((uintptr_t)tag->size + 7) & ~(uintptr_t)7;
        if (advance > end - cursor) return false;
        cursor += advance;
    }
    return false;
}

static bool cmdline_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool multiboot2_has_cmdline_token(multiboot2_info_t* mb_info,
                                  const char* token) {
    if (!mb_info || !token || !*token || mb_info->total_size < sizeof(*mb_info)) {
        return false;
    }

    size_t token_length = 0;
    while (token[token_length]) token_length++;

    uintptr_t start = (uintptr_t)mb_info;
    if (mb_info->total_size > UINTPTR_MAX - start) return false;
    uintptr_t end = start + mb_info->total_size;
    uintptr_t cursor = (uintptr_t)mb_info->tags;

    while (cursor <= end && end - cursor >= sizeof(multiboot2_tag_t)) {
        const multiboot2_tag_t* tag = (const multiboot2_tag_t*)cursor;
        if (tag->size < sizeof(*tag) || tag->size > end - cursor) return false;
        if (tag->type == 0) return false;

        if (tag->type == 1) {
            const char* command_line = (const char*)(tag + 1);
            size_t command_length = tag->size - sizeof(*tag);
            size_t position = 0;
            while (position < command_length && command_line[position]) {
                while (position < command_length &&
                       cmdline_is_space(command_line[position])) position++;
                size_t word_start = position;
                while (position < command_length && command_line[position] &&
                       !cmdline_is_space(command_line[position])) position++;
                if (position - word_start == token_length) {
                    size_t i = 0;
                    while (i < token_length &&
                           command_line[word_start + i] == token[i]) i++;
                    if (i == token_length) return true;
                }
            }
            return false;
        }

        uintptr_t advance = ((uintptr_t)tag->size + 7) & ~(uintptr_t)7;
        if (advance > end - cursor) return false;
        cursor += advance;
    }
    return false;
}
