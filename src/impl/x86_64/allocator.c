#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/allocator.h"
#include "serial.h"
#include "panic.h"
#include "string.h"

#define ALIGNMENT 16  // Better alignment for modern CPUs
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Block header structure
typedef struct block_header {
    size_t size;               // Size of usable data (NOT including header)
    struct block_header* next; // Next free block
    struct block_header* prev; // Previous free block (for faster coalescing)
    bool free;
    uint32_t magic;
#ifdef ALLOCATOR_DEBUG
    const char* file;          // Allocation source file
    int line;                  // Allocation source line
#endif
} __attribute__((aligned(ALIGNMENT))) block_header;

#define BLOCK_MAGIC 0xDEADBEEF
#define HEADER_SIZE (sizeof(block_header))
#define MIN_BLOCK_SIZE 32  // Minimum allocation size to reduce fragmentation

static block_header* free_list_head = NULL;
static void* heap_start = NULL;
static void* heap_end = NULL;
static size_t total_allocated = 0;
static size_t total_free = 0;
static size_t peak_usage = 0;

static inline uintptr_t align_up_uintptr(uintptr_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline bool is_power_of_two(size_t x) {
    return x && ((x & (x - 1)) == 0);
}

static size_t next_power_of_two(size_t x) {
    if (x == 0) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
#if UINTPTR_MAX > 0xFFFFFFFF
    x |= x >> 32;
#endif
    return x + 1;
}

// Statistics
static struct {
    size_t alloc_count;
    size_t free_count;
    size_t split_count;
    size_t coalesce_count;
    size_t failed_allocs;
} stats = {0};

// Initialize allocator
void allocator_init(void* memory, size_t size) {
    serial_write_str("Allocator: Initializing with ");
    serial_write_dec(size);
    serial_write_str(" bytes at 0x");
    serial_write_hex((uintptr_t)memory);
    serial_write_str("\n");
    
    if (!memory || size < HEADER_SIZE + MIN_BLOCK_SIZE) {
        PANIC("allocator_init: invalid parameters");
    }
    
    // Align memory start
    uintptr_t aligned_start = ALIGN((uintptr_t)memory);
    size_t alignment_loss = aligned_start - (uintptr_t)memory;
    
    if (alignment_loss > 0) {
        serial_write_str("Allocator: Aligned start by ");
        serial_write_dec(alignment_loss);
        serial_write_str(" bytes\n");
        memory = (void*)aligned_start;
        size -= alignment_loss;
    }
    
    heap_start = memory;
    heap_end = (void*)((uintptr_t)memory + size);
    
    // Initialize first block - don't zero the entire heap!
    free_list_head = (block_header*)memory;
    free_list_head->size = size - HEADER_SIZE;  // Size = usable space
    free_list_head->free = true;
    free_list_head->next = NULL;
    free_list_head->prev = NULL;
    free_list_head->magic = BLOCK_MAGIC;
    
    total_free = free_list_head->size;
    total_allocated = 0;
    peak_usage = 0;
    
    serial_write_str("Allocator: Ready. Free space: ");
    serial_write_dec(total_free);
    serial_write_str(" bytes\n");
}

// Fast validation (only check critical fields)
static inline bool validate_block_fast(block_header* block) {
    return block && 
           block->magic == BLOCK_MAGIC &&
           (void*)block >= heap_start && 
           (void*)block < heap_end;
}

// Full validation (for debugging)
static bool validate_block_full(block_header* block) {
    if (!block) {
        serial_write_str("ERROR: NULL block\n");
        return false;
    }
    
    if ((void*)block < heap_start || (void*)block >= heap_end) {
        serial_write_str("ERROR: Block outside heap: 0x");
        serial_write_hex((uintptr_t)block);
        serial_write_str("\n");
        return false;
    }
    
    if (block->magic != BLOCK_MAGIC) {
        serial_write_str("ERROR: Invalid magic 0x");
        serial_write_hex(block->magic);
        serial_write_str(" at 0x");
        serial_write_hex((uintptr_t)block);
        serial_write_str("\n");
        return false;
    }
    
    uintptr_t block_end = (uintptr_t)block + HEADER_SIZE + block->size;
    if (block_end > (uintptr_t)heap_end) {
        serial_write_str("ERROR: Block extends beyond heap\n");
        return false;
    }
    
    return true;
}

// Split block if large enough
static void split_block(block_header* block, size_t size) {
    size = ALIGN(size);
    
    // Can we split? Need space for new header + minimum block size
    size_t remaining = block->size - size;
    
    if (remaining >= HEADER_SIZE + MIN_BLOCK_SIZE) {
        // Create new free block
        block_header* new_block = (block_header*)((uint8_t*)block + HEADER_SIZE + size);
        new_block->size = remaining - HEADER_SIZE;
        new_block->free = true;
        new_block->magic = BLOCK_MAGIC;
        new_block->next = block->next;
        new_block->prev = block->prev;
        
        // Update free list
        if (block->prev) {
            block->prev->next = new_block;
        } else {
            free_list_head = new_block;
        }
        
        if (block->next) {
            block->next->prev = new_block;
        }
        
        // Update original block
        block->size = size;
        block->free = false;
        block->next = NULL;
        block->prev = NULL;
        
        stats.split_count++;
    } else {
        // Can't split - allocate entire block
        if (block->prev) {
            block->prev->next = block->next;
        } else {
            free_list_head = block->next;
        }
        
        if (block->next) {
            block->next->prev = block->prev;
        }
        
        block->free = false;
        block->next = NULL;
        block->prev = NULL;
    }
}

// Allocate memory
void* alloc(size_t size) {
    if (size == 0) return NULL;
    
    // Enforce minimum size and alignment
    if (size < MIN_BLOCK_SIZE) size = MIN_BLOCK_SIZE;
    size = ALIGN(size);
    
    block_header* best_fit = NULL;
    block_header* curr = free_list_head;
    
    // First-fit with size check
    while (curr) {
        if (!validate_block_fast(curr)) {
            serial_write_str("ERROR: Corrupted free list during alloc\n");
            allocator_debug();
            PANIC("alloc: corrupted heap");
        }
        
        if (curr->free && curr->size >= size) {
            best_fit = curr;
            break;  // First fit for speed
        }
        
        curr = curr->next;
    }
    
    if (!best_fit) {
        serial_write_str("ALLOC FAILED: Requested ");
        serial_write_dec(size);
        serial_write_str(" bytes, ");
        serial_write_dec(total_free);
        serial_write_str(" free\n");
        stats.failed_allocs++;
        return NULL;
    }
    
    // Split and allocate
    split_block(best_fit, size);
    
    void* ptr = (void*)((uint8_t*)best_fit + HEADER_SIZE);
    
    // Zero the memory
    memset(ptr, 0, size);
    
    // Update stats
    total_allocated += size;
    total_free -= size;
    if (total_allocated > peak_usage) {
        peak_usage = total_allocated;
    }
    stats.alloc_count++;
    
    return ptr;
}

// Insert block into free list (sorted by address)
static void insert_free_block(block_header* block) {
    if (!validate_block_fast(block)) {
        PANIC("insert_free_block: invalid block");
    }
    
    block->free = true;
    
    // Empty list
    if (!free_list_head) {
        free_list_head = block;
        block->next = NULL;
        block->prev = NULL;
        return;
    }
    
    // Insert at head
    if (block < free_list_head) {
        block->next = free_list_head;
        block->prev = NULL;
        free_list_head->prev = block;
        free_list_head = block;
        return;
    }
    
    // Find insertion point
    block_header* curr = free_list_head;
    while (curr->next && curr->next < block) {
        curr = curr->next;
    }
    
    // Insert after curr
    block->next = curr->next;
    block->prev = curr;
    
    if (curr->next) {
        curr->next->prev = block;
    }
    curr->next = block;
}

// Coalesce adjacent free blocks
static void coalesce(void) {
    block_header* curr = free_list_head;
    
    while (curr) {
        if (!validate_block_fast(curr)) {
            PANIC("coalesce: corrupted block");
        }
        
        // Try to merge with next block
        if (curr->next) {
            uintptr_t curr_end = (uintptr_t)curr + HEADER_SIZE + curr->size;
            
            if (curr_end == (uintptr_t)curr->next) {
                // Adjacent! Merge them
                curr->size += HEADER_SIZE + curr->next->size;
                
                block_header* next_next = curr->next->next;
                curr->next = next_next;
                
                if (next_next) {
                    next_next->prev = curr;
                }
                
                stats.coalesce_count++;
                // Don't advance - check again
                continue;
            }
        }
        
        curr = curr->next;
    }
}

void* kmalloc_aligned(size_t size, size_t alignment) {
    if (size == 0) return NULL;

    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if (!is_power_of_two(alignment)) alignment = next_power_of_two(alignment);

    size_t total = size + alignment - 1 + sizeof(void*);
    void* raw = alloc(total);
    if (!raw) return NULL;

    uintptr_t raw_addr = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned_addr = align_up_uintptr(raw_addr, alignment);

    ((void**)aligned_addr)[-1] = raw;
    return (void*)aligned_addr;
}

void kfree_aligned(void* ptr) {
    if (!ptr) return;
    void* raw = ((void**)ptr)[-1];
    free_mem(raw);
}

// Free memory
void free_mem(void* ptr) {
    if (!ptr) return;
    
    if (ptr < heap_start || ptr >= heap_end) {
        serial_write_str("ERROR: free_mem of invalid pointer 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        return;  // Don't panic - just ignore
    }
    
    block_header* block = (block_header*)((uint8_t*)ptr - HEADER_SIZE);
    
    if (!validate_block_full(block)) {
        serial_write_str("ERROR: Corrupted block header in free_mem\n");
        return;  // Don't panic
    }
    
    if (block->free) {
        serial_write_str("WARNING: Double free at 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        return;
    }
    
    // Update stats
    total_allocated -= block->size;
    total_free += block->size;
    stats.free_count++;
    
    insert_free_block(block);
    coalesce();
}

// Allocator statistics
void allocator_stats(void) {
    serial_write_str("=== Allocator Statistics ===\n");
    serial_write_str("Total allocated: ");
    serial_write_dec(total_allocated);
    serial_write_str(" bytes\n");
    serial_write_str("Total free:      ");
    serial_write_dec(total_free);
    serial_write_str(" bytes\n");
    serial_write_str("Peak usage:      ");
    serial_write_dec(peak_usage);
    serial_write_str(" bytes\n");
    serial_write_str("Allocations:     ");
    serial_write_dec(stats.alloc_count);
    serial_write_str("\n");
    serial_write_str("Frees:           ");
    serial_write_dec(stats.free_count);
    serial_write_str("\n");
    serial_write_str("Failed allocs:   ");
    serial_write_dec(stats.failed_allocs);
    serial_write_str("\n");
    serial_write_str("Splits:          ");
    serial_write_dec(stats.split_count);
    serial_write_str("\n");
    serial_write_str("Coalesces:       ");
    serial_write_dec(stats.coalesce_count);
    serial_write_str("\n");
}

// Debug allocator state
void allocator_debug(void) {
    serial_write_str("=== Allocator Debug ===\n");
    serial_write_str("Heap: 0x");
    serial_write_hex((uintptr_t)heap_start);
    serial_write_str(" - 0x");
    serial_write_hex((uintptr_t)heap_end);
    serial_write_str("\n");
    
    serial_write_str("Free blocks:\n");
    block_header* curr = free_list_head;
    int count = 0;
    
    while (curr && count < 50) {
        serial_write_str("  [");
        serial_write_dec(count);
        serial_write_str("] 0x");
        serial_write_hex((uintptr_t)curr);
        serial_write_str(" size=");
        serial_write_dec(curr->size);
        serial_write_str("\n");
        
        curr = curr->next;
        count++;
    }
    
    if (count >= 50) {
        serial_write_str("  ... (truncated)\n");
    }
    
    serial_write_str("=======================\n");
}
