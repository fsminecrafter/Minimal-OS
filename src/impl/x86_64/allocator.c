#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/allocator.h"
#include "serial.h"
#include "panic.h"
#include "string.h"

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Block header structure
typedef struct block_header {
    size_t size;               // Size includes header
    struct block_header* next; // Next free block in list
    bool free;                 // Is block free or allocated
    uint32_t magic;            // Magic number for validation (0xDEADBEEF)
} block_header;

#define BLOCK_MAGIC 0xDEADBEEF
#define HEADER_SIZE (sizeof(block_header))

static block_header* free_list_head = NULL;
static void* heap_start = NULL;
static void* heap_end = NULL;

// Initialize allocator with memory region
void allocator_init(void* memory, size_t size) {
    serial_write_str("Initializing allocator...\n");
    
    if (!memory || size < HEADER_SIZE + ALIGNMENT) {
        PANIC("allocator_init: invalid parameters");
    }
    
    // Check alignment
    if (((uintptr_t)memory & (ALIGNMENT - 1)) != 0) {
        serial_write_str("Warning: memory not aligned, aligning...\n");
        memory = (void*)ALIGN((uintptr_t)memory);
        size -= ((uintptr_t)memory & (ALIGNMENT - 1));
    }
    
    serial_write_str("Allocator memory: ");
    serial_write_hex((uintptr_t)memory);
    serial_write_str(" - ");
    serial_write_hex((uintptr_t)memory + size);
    serial_write_str(" (");
    serial_write_dec(size);
    serial_write_str(" bytes)\n");
    
    heap_start = memory;
    heap_end = (void*)((uintptr_t)memory + size);
    
    // For large heaps (>1MB), skip zeroing - just initialize the header
    // The memory will be zeroed when individual blocks are allocated
    if (size > 1024 * 1024) {
        serial_write_str("Large heap detected, skipping full zero (will zero on allocation)\n");
        
        // Just zero the first block header
        free_list_head = (block_header*)memory;
        free_list_head->size = size;
        free_list_head->free = true;
        free_list_head->next = NULL;
        free_list_head->magic = BLOCK_MAGIC;
    } else {
        // For small heaps, zero everything for safety
        serial_write_str("Zeroing allocator memory...\n");
        memset(memory, 0, size);
        serial_write_str("Memory zeroed\n");
        
        // Initialize the first free block
        free_list_head = (block_header*)memory;
        free_list_head->size = size;
        free_list_head->free = true;
        free_list_head->next = NULL;
        free_list_head->magic = BLOCK_MAGIC;
    }
    
    serial_write_str("Allocator initialized successfully\n");
}

// Validate block header
static bool validate_block(block_header* block) {
    if (!block) return false;
    
    // Check if block is within heap bounds
    if ((void*)block < heap_start || (void*)block >= heap_end) {
        serial_write_str("ERROR: Block outside heap: ");
        serial_write_hex((uintptr_t)block);
        serial_write_str("\n");
        return false;
    }
    
    // Check magic number
    if (block->magic != BLOCK_MAGIC) {
        serial_write_str("ERROR: Block has invalid magic: ");
        serial_write_hex(block->magic);
        serial_write_str(" at ");
        serial_write_hex((uintptr_t)block);
        serial_write_str("\n");
        return false;
    }
    
    // Check size is reasonable
    if (block->size < HEADER_SIZE || block->size > (size_t)((uintptr_t)heap_end - (uintptr_t)heap_start)) {
        serial_write_str("ERROR: Block has invalid size: ");
        serial_write_dec(block->size);
        serial_write_str(" at ");
        serial_write_hex((uintptr_t)block);
        serial_write_str("\n");
        return false;
    }
    
    return true;
}

// Split block if big enough
void split_block(block_header* block, size_t size) {
    if (!validate_block(block)) {
        PANIC("split_block: invalid block");
    }
    
    // Check if we can split: remaining size after allocation must be enough for a new block + header
    if (block->size >= size + HEADER_SIZE + ALIGNMENT) {
        block_header* new_block = (block_header*)((uint8_t*)block + size);
        new_block->size = block->size - size;
        new_block->free = true;
        new_block->next = block->next;
        new_block->magic = BLOCK_MAGIC;

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
    if (size == 0) {
        serial_write_str("WARNING: alloc called with size 0\n");
        return NULL;
    }
    
    size_t total_size = ALIGN(size) + HEADER_SIZE;

    block_header* curr = free_list_head;

    // Find first fit block
    while (curr) {
        if (!validate_block(curr)) {
            PANIC("alloc: corrupted free list");
        }
        
        if (curr->free && curr->size >= total_size) {
            // Found block
            split_block(curr, total_size);
            
            void* ptr = (void*)((uint8_t*)curr + HEADER_SIZE);
            
            // Zero the allocated memory for safety
            memset(ptr, 0, size);
            
            return ptr;
        }
        curr = curr->next;
    }
    
    // No suitable block found
    serial_write_str("ERROR: alloc failed - no suitable block for size ");
    serial_write_dec(size);
    serial_write_str("\n");
    
    return NULL;
}

// Insert block into free list in sorted order (by address)
void insert_free_block(block_header* block) {
    if (!validate_block(block)) {
        PANIC("insert_free_block: invalid block");
    }
    
    block_header** curr = &free_list_head;

    while (*curr && *curr < block) {
        if (!validate_block(*curr)) {
            PANIC("insert_free_block: corrupted free list");
        }
        curr = &(*curr)->next;
    }
    block->next = *curr;
    *curr = block;
}

// Coalesce adjacent free blocks
void coalesce() {
    block_header* curr = free_list_head;

    while (curr && curr->next) {
        if (!validate_block(curr) || !validate_block(curr->next)) {
            PANIC("coalesce: corrupted free list");
        }
        
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
    if (ptr == NULL) {
        serial_write_str("WARNING: free_mem called with NULL\n");
        return;
    }
    
    // Check if pointer is within heap
    if (ptr < heap_start || ptr >= heap_end) {
        serial_write_str("ERROR: Attempting to free pointer outside heap: ");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        PANIC("free_mem: invalid pointer");
    }
    
    block_header* block = (block_header*)((uint8_t*)ptr - HEADER_SIZE);
    
    if (!validate_block(block)) {
        PANIC("free_mem: invalid block header");
    }
    
    if (block->free) {
        serial_write_str("WARNING: Double free detected at ");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        return;
    }
    
    block->free = true;

    insert_free_block(block);
    coalesce();
}

// Debug function to print allocator state
void allocator_debug(void) {
    serial_write_str("=== Allocator Debug ===\n");
    serial_write_str("Heap: ");
    serial_write_hex((uintptr_t)heap_start);
    serial_write_str(" - ");
    serial_write_hex((uintptr_t)heap_end);
    serial_write_str("\n");
    
    serial_write_str("Free list:\n");
    block_header* curr = free_list_head;
    int count = 0;
    
    while (curr && count < 100) {  // Limit to prevent infinite loops
        serial_write_str("  Block ");
        serial_write_dec(count);
        serial_write_str(": addr=");
        serial_write_hex((uintptr_t)curr);
        serial_write_str(" size=");
        serial_write_dec(curr->size);
        serial_write_str(" free=");
        serial_write_dec(curr->free ? 1 : 0);
        serial_write_str("\n");
        
        curr = curr->next;
        count++;
    }
    
    if (count >= 100) {
        serial_write_str("WARNING: Free list might be corrupted (>100 blocks)\n");
    }
    
    serial_write_str("======================\n");
}