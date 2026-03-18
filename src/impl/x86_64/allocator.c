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
#define FOOTER_MAGIC 0xBAADF00D
#define FOOTER_SIZE (sizeof(uint32_t))
#define HEADER_SIZE (sizeof(block_header))
#define MIN_BLOCK_SIZE 32  // Minimum allocation size to reduce fragmentation

static block_header* free_list_head = NULL;
static void* heap_start = NULL;
static void* heap_end = NULL;
static size_t total_allocated = 0;
static size_t total_free = 0;
static size_t peak_usage = 0;
static size_t heap_size = 0;
static volatile int allocator_spin = 0;

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static inline uint64_t allocator_lock(void) {
    uint64_t flags = irq_save();
    while (__sync_lock_test_and_set(&allocator_spin, 1)) {
        // spin
    }
    return flags;
}

static inline void allocator_unlock(uint64_t flags) {
    __sync_lock_release(&allocator_spin);
    irq_restore(flags);
}

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
    
    // Align size down to keep block sizes aligned
    size &= ~(ALIGNMENT - 1);
    
    heap_start = memory;
    heap_end = (void*)((uintptr_t)memory + size);
    heap_size = size;
    allocator_spin = 0;
    
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
    if (!block) return false;
    if (block->magic != BLOCK_MAGIC) return false;
    if ((void*)block < heap_start || (void*)block >= heap_end) return false;
    if (block->size == 0 || block->size < MIN_BLOCK_SIZE) return false;
    if ((block->size & (ALIGNMENT - 1)) != 0) return false;
    if (block->next && ((void*)block->next < heap_start || (void*)block->next >= heap_end)) return false;
    if (block->prev && ((void*)block->prev < heap_start || (void*)block->prev >= heap_end)) return false;
    uintptr_t block_end = (uintptr_t)block + HEADER_SIZE + block->size;
    if (block_end < (uintptr_t)block) return false;
    if (block_end > (uintptr_t)heap_end) return false;
    return true;
}

static void dump_block(const char* label, block_header* block) {
    serial_write_str(label);
    serial_write_str(" 0x");
    serial_write_hex((uintptr_t)block);
    serial_write_str("\n");

    if ((void*)block < heap_start || (void*)block >= heap_end) {
        serial_write_str("  (outside heap, cannot read fields safely)\n");
        return;
    }

    serial_write_str("  size=");
    serial_write_dec(block->size);
    serial_write_str(" free=");
    serial_write_dec(block->free ? 1 : 0);
    serial_write_str(" magic=0x");
    serial_write_hex(block->magic);
    serial_write_str("\n  next=0x");
    serial_write_hex((uintptr_t)block->next);
    serial_write_str(" prev=0x");
    serial_write_hex((uintptr_t)block->prev);
    serial_write_str("\n");
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
    
    if (block->size == 0 || block->size < MIN_BLOCK_SIZE || (block->size & (ALIGNMENT - 1)) != 0) {
        serial_write_str("ERROR: Invalid block size ");
        serial_write_dec(block->size);
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
    size_t total_size = ALIGN(size + FOOTER_SIZE);
    
    uint64_t flags = allocator_lock();
    
    block_header* best_fit = NULL;
    block_header* curr = free_list_head;
    size_t max_blocks = heap_size / (HEADER_SIZE + MIN_BLOCK_SIZE) + 1;
    size_t visited = 0;
    
    // First-fit with size check
    while (curr) {
        if (++visited > max_blocks) {
            serial_write_str("ERROR: Free list cycle detected during alloc\n");
            allocator_debug();
            PANIC("alloc: free list cycle");
        }
        if (!validate_block_fast(curr)) {
            serial_write_str("ERROR: Corrupted free list during alloc\n");
            dump_block("  bad block:", curr);
            allocator_debug();
            PANIC("alloc: corrupted heap");
        }
        
        if (curr->free && curr->size >= total_size) {
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
        allocator_unlock(flags);
        return NULL;
    }
    
    // Split and allocate
    split_block(best_fit, total_size);
    
    void* ptr = (void*)((uint8_t*)best_fit + HEADER_SIZE);
    
    // Update stats
    total_allocated += total_size;
    total_free -= total_size;
    if (total_allocated > peak_usage) {
        peak_usage = total_allocated;
    }
    stats.alloc_count++;
    allocator_unlock(flags);
    
    // Zero the memory and place footer guard
    memset(ptr, 0, total_size - FOOTER_SIZE);
    uint32_t* footer = (uint32_t*)((uint8_t*)ptr + (total_size - FOOTER_SIZE));
    *footer = FOOTER_MAGIC;
    
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
    size_t max_blocks = heap_size / (HEADER_SIZE + MIN_BLOCK_SIZE) + 1;
    size_t visited = 0;
    
    while (curr) {
        if (++visited > max_blocks) {
            PANIC("coalesce: free list cycle");
        }
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
    
    uint64_t flags = allocator_lock();
    if (ptr < heap_start || ptr >= heap_end) {
        serial_write_str("ERROR: free_mem of invalid pointer 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        allocator_unlock(flags);
        return;  // Don't panic - just ignore
    }
    
    block_header* block = (block_header*)((uint8_t*)ptr - HEADER_SIZE);
    
    if (!validate_block_full(block)) {
        serial_write_str("ERROR: Corrupted block header in free_mem\n");
        allocator_unlock(flags);
        return;  // Don't panic
    }
    
    if (block->free) {
        serial_write_str("WARNING: Double free at 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        allocator_unlock(flags);
        return;
    }
    
    uint32_t* footer = (uint32_t*)((uint8_t*)block + HEADER_SIZE + block->size - FOOTER_SIZE);
    if (*footer != FOOTER_MAGIC) {
        serial_write_str("ERROR: Heap overflow detected at 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str(" (footer=0x");
        serial_write_hex(*footer);
        serial_write_str(")\n");
        dump_block("  owning block:", block);
        allocator_unlock(flags);
        PANIC("free_mem: heap overflow");
    }
    
    // Update stats
    total_allocated -= block->size;
    total_free += block->size;
    stats.free_count++;
    
    insert_free_block(block);
    coalesce();
    allocator_unlock(flags);
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
