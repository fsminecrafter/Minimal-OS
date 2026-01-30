#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/allocator.h"
#include "serial.h"
#include "panic.h"

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Block header structure
typedef struct block_header {
    size_t size;               // Size includes header
    struct block_header* next; // Next free block in list
    bool free;                 // Is block free or allocated
} block_header;

static block_header* free_list_head = NULL;

#define HEADER_SIZE (sizeof(block_header))

// Initialize allocator with memory region
void allocator_init(void* memory, size_t size) {
    serial_write_str("Initializing memory\n");
    // The entire memory is one big free block
    free_list_head = (block_header*)memory;
    free_list_head->size = size;
    free_list_head->free = true;
    free_list_head->next = NULL;
}

// Split block if big enough
void split_block(block_header* block, size_t size) {
    // Check if we can split: remaining size after allocation must be enough for a new block + header
    if (block->size >= size + HEADER_SIZE + ALIGNMENT) {
        block_header* new_block = (block_header*)((uint8_t*)block + size);
        new_block->size = block->size - size;
        new_block->free = true;
        new_block->next = block->next;

        block->size = size;
        block->free = false;
        block->next = NULL;

        // Insert new block into free list in place of old block
        // Find predecessor to block in free list
        block_header** curr = &free_list_head;
        while (*curr && *curr != block) {
            curr = &(*curr)->next;
        }
        if (*curr == block) {
            *curr = new_block;
        }
    } else {
        // Can't split: allocate entire block
        block->free = false;
        // Remove from free list
        block_header** curr = &free_list_head;
        while (*curr && *curr != block) {
            curr = &(*curr)->next;
        }
        if (*curr == block) {
            *curr = block->next;
        }
        block->next = NULL;
    }
}

// Allocate memory
void* alloc(size_t size) {
    serial_write_str("Allocating...\n");
    
    if (size == 0) PANIC("size is weird");
    size_t total_size = ALIGN(size) + HEADER_SIZE;

    block_header* prev = NULL;
    block_header* curr = free_list_head;

    // Find first fit block
    while (curr) {
        if (curr->free && curr->size >= total_size) {
            // Found block
            split_block(curr, total_size);
            serial_write_str("Success.\n");
            return (void*)((uint8_t*)curr + HEADER_SIZE);
        }
        prev = curr;
        curr = curr->next;
    }
    serial_write_str("Failure.\n");
    // No suitable block found
    PANIC("No sutiable blocks");
}

// Insert block into free list in sorted order (by address)
void insert_free_block(block_header* block) {
    block_header** curr = &free_list_head;

    while (*curr && *curr < block) {
        curr = &(*curr)->next;
    }
    block->next = *curr;
    *curr = block;
}

// Coalesce adjacent free blocks
void coalesce() {
    block_header* curr = free_list_head;

    while (curr && curr->next) {
        uint8_t* curr_end = (uint8_t*)curr + curr->size;
        if (curr_end == (uint8_t*)curr->next) {
            // Adjacent blocks - merge
            curr->size += curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

// Free memory
void free_mem(void* ptr) {
    if (ptr == NULL) return;
    serial_write_str("Freeing memory.\n");
    block_header* block = (block_header*)((uint8_t*)ptr - HEADER_SIZE);
    block->free = true;

    insert_free_block(block);
    coalesce();
}
