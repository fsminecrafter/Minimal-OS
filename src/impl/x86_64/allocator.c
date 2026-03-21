#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/allocator.h"
#include "serial.h"
#include "panic.h"
#include "string.h"

// ===========================================
// CONFIGURATION
// ===========================================

#define ALIGNMENT       16      // All allocations aligned to 16 bytes (x86-64 ABI)
#define MIN_BLOCK_SIZE  32      // Minimum usable bytes per block (reduces fragmentation)
#define BLOCK_MAGIC     0xDEADBEEF
#define FOOTER_MAGIC    0xBAADF00D

#define ALIGN(n)        (((n) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))

// ===========================================
// BLOCK LAYOUT
//
//  [block_header | ... usable data ... | uint32_t footer]
//   ^                                   ^
//   returned ptr - HEADER_SIZE          footer guard
//
// block->size = total bytes from end-of-header to end-of-footer (inclusive).
// usable bytes for caller = block->size - FOOTER_SIZE.
// ===========================================

typedef struct block_header {
    size_t size;                // Total block payload (usable + footer). Always ALIGN'd.
    struct block_header* next;  // Next block in free list (NULL if tail)
    struct block_header* prev;  // Previous block in free list (NULL if head)
    bool   free;
    uint32_t magic;
} __attribute__((aligned(ALIGNMENT))) block_header;

#define HEADER_SIZE  (sizeof(block_header))
#define FOOTER_SIZE  (sizeof(uint32_t))

// ===========================================
// GLOBAL STATE
// ===========================================

static block_header* free_list_head = NULL;
static void*   heap_start   = NULL;
static void*   heap_end     = NULL;
static size_t  heap_size    = 0;

static size_t  total_allocated = 0;
static size_t  total_free      = 0;
static size_t  peak_usage      = 0;

static volatile int allocator_spin = 0;

static struct {
    size_t alloc_count;
    size_t free_count;
    size_t split_count;
    size_t coalesce_count;
    size_t failed_allocs;
    size_t resize_count;
} stats = {0};

// ===========================================
// LOCKING  (IRQ-safe spinlock)
// ===========================================

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
    while (__sync_lock_test_and_set(&allocator_spin, 1)) { /* spin */ }
    return flags;
}

static inline void allocator_unlock(uint64_t flags) {
    __sync_lock_release(&allocator_spin);
    irq_restore(flags);
}

// ===========================================
// FOOTER HELPERS
// ===========================================

static inline uint32_t* block_footer(block_header* b) {
    return (uint32_t*)((uint8_t*)b + HEADER_SIZE + b->size - FOOTER_SIZE);
}

static inline void write_footer(block_header* b) {
    *block_footer(b) = FOOTER_MAGIC;
}

static inline bool check_footer(block_header* b) {
    return *block_footer(b) == FOOTER_MAGIC;
}

// ===========================================
// BLOCK VALIDATION
// ===========================================

static inline bool block_in_heap(block_header* b) {
    if (!b) return false;
    uintptr_t addr = (uintptr_t)b;
    uintptr_t end  = addr + HEADER_SIZE + b->size;
    return (void*)b  >= heap_start &&
           (void*)b  <  heap_end   &&
           end       <= (uintptr_t)heap_end &&
           end        > addr;  // overflow check
}

// Fast inline check — used in hot paths
static inline bool validate_block_fast(block_header* b) {
    if (!b) return false;
    if (b->magic != BLOCK_MAGIC) return false;
    if (!block_in_heap(b)) return false;
    if (b->size < MIN_BLOCK_SIZE) return false;
    if (b->size & (ALIGNMENT - 1)) return false;
    return true;
}

// Full check with serial diagnostics — used on error/free paths
static bool validate_block_full(block_header* b, const char* context) {
    if (!b) {
        serial_write_str("HEAP ERROR ["); serial_write_str(context);
        serial_write_str("]: NULL block\n");
        return false;
    }
    if (b->magic != BLOCK_MAGIC) {
        serial_write_str("HEAP ERROR ["); serial_write_str(context);
        serial_write_str("]: bad magic 0x");
        serial_write_hex(b->magic);
        serial_write_str(" at 0x"); serial_write_hex((uintptr_t)b);
        serial_write_str("\n");
        return false;
    }
    if (!block_in_heap(b)) {
        serial_write_str("HEAP ERROR ["); serial_write_str(context);
        serial_write_str("]: block 0x"); serial_write_hex((uintptr_t)b);
        serial_write_str(" outside heap\n");
        return false;
    }
    if (b->size < MIN_BLOCK_SIZE || (b->size & (ALIGNMENT - 1))) {
        serial_write_str("HEAP ERROR ["); serial_write_str(context);
        serial_write_str("]: bad size "); serial_write_dec(b->size);
        serial_write_str(" at 0x"); serial_write_hex((uintptr_t)b);
        serial_write_str("\n");
        return false;
    }
    return true;
}

// ===========================================
// FREE LIST MANAGEMENT
// ===========================================

// Insert block into free list, sorted by address (enables O(n) coalescing)
static void insert_free_block(block_header* b) {
    b->free = true;

    if (!free_list_head || b < free_list_head) {
        b->next = free_list_head;
        b->prev = NULL;
        if (free_list_head) free_list_head->prev = b;
        free_list_head = b;
        return;
    }

    block_header* curr = free_list_head;
    while (curr->next && curr->next < b) {
        curr = curr->next;
    }

    b->next = curr->next;
    b->prev = curr;
    if (curr->next) curr->next->prev = b;
    curr->next = b;
}

// Remove block from free list
static void remove_free_block(block_header* b) {
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        free_list_head = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    b->next = NULL;
    b->prev = NULL;
    b->free = false;
}

// Coalesce b with its immediate successor if adjacent and free
static void try_merge_with_next(block_header* b) {
    if (!b->next) return;
    uintptr_t b_end = (uintptr_t)b + HEADER_SIZE + b->size;
    if (b_end != (uintptr_t)b->next) return;

    // They're adjacent — merge
    block_header* next = b->next;
    b->size += HEADER_SIZE + next->size;
    b->next = next->next;
    if (next->next) next->next->prev = b;

    // Overwrite next's magic so stale pointers to it are caught
    next->magic = 0xDEAD0000;
    stats.coalesce_count++;
}

// Full coalesce pass over the free list
static void coalesce(void) {
    block_header* curr = free_list_head;
    size_t max = heap_size / (HEADER_SIZE + MIN_BLOCK_SIZE) + 1;
    size_t visited = 0;

    while (curr) {
        if (++visited > max) PANIC("coalesce: free list cycle");
        if (!validate_block_fast(curr)) PANIC("coalesce: corrupted block");
        try_merge_with_next(curr);
        curr = curr->next;
    }
}

// ===========================================
// SPLIT
// Carve `need` bytes out of `b`. `need` must already be ALIGN'd and
// include the footer. Removes b from free list, updates stats.
// ===========================================

static void split_and_use(block_header* b, size_t need) {
    size_t leftover = b->size - need;

    if (leftover >= HEADER_SIZE + MIN_BLOCK_SIZE) {
        // Create a new free block from the remainder
        block_header* rest = (block_header*)((uint8_t*)b + HEADER_SIZE + need);
        rest->size  = leftover - HEADER_SIZE;
        rest->magic = BLOCK_MAGIC;
        rest->next  = NULL;
        rest->prev  = NULL;
        rest->free  = false;  // insert_free_block sets it to true
        write_footer(rest);

        // Splice rest into the free list in place of b
        rest->next = b->next;
        rest->prev = b->prev;
        if (b->prev) b->prev->next = rest; else free_list_head = rest;
        if (b->next) b->next->prev = rest;
        rest->free = true;

        b->size = need;
        stats.split_count++;
    } else {
        // Use the whole block
        remove_free_block(b);
    }

    b->free = false;
    b->next = NULL;
    b->prev = NULL;
    write_footer(b);
}

// ===========================================
// ALLOCATOR INIT
// ===========================================

void allocator_init(void* memory, size_t size) {
    if (!memory || size < HEADER_SIZE + MIN_BLOCK_SIZE + FOOTER_SIZE) {
        PANIC("allocator_init: invalid parameters");
    }

    // Align start address up
    uintptr_t start = ALIGN((uintptr_t)memory);
    size_t lost = start - (uintptr_t)memory;
    if (lost > size) PANIC("allocator_init: region too small after alignment");
    memory = (void*)start;
    size   = (size - lost) & ~(size_t)(ALIGNMENT - 1);

    heap_start = memory;
    heap_end   = (void*)((uintptr_t)memory + size);
    heap_size  = size;
    allocator_spin   = 0;
    total_allocated  = 0;
    total_free       = 0;
    peak_usage       = 0;

    // Single large free block covering the whole heap
    free_list_head = (block_header*)memory;
    free_list_head->size  = size - HEADER_SIZE;
    free_list_head->free  = true;
    free_list_head->next  = NULL;
    free_list_head->prev  = NULL;
    free_list_head->magic = BLOCK_MAGIC;
    write_footer(free_list_head);

    total_free = free_list_head->size;

    serial_write_str("Allocator: Ready. Heap=");
    serial_write_hex((uintptr_t)heap_start);
    serial_write_str("-");
    serial_write_hex((uintptr_t)heap_end);
    serial_write_str(", free=");
    serial_write_dec(total_free);
    serial_write_str(" bytes\n");
}

// ===========================================
// INTERNAL ALLOC CORE (called with lock held)
// ===========================================

// Rounds `size` up to a safe total payload size (usable + footer, ALIGN'd)
static inline size_t payload_size(size_t user_size) {
    if (user_size < MIN_BLOCK_SIZE) user_size = MIN_BLOCK_SIZE;
    return ALIGN(user_size + FOOTER_SIZE);
}

static void* alloc_locked(size_t total) {
    // First-fit search
    block_header* curr = free_list_head;
    size_t max = heap_size / (HEADER_SIZE + MIN_BLOCK_SIZE) + 1;
    size_t visited = 0;

    while (curr) {
        if (++visited > max) {
            serial_write_str("HEAP ERROR: free list cycle during alloc\n");
            PANIC("alloc: free list cycle");
        }
        if (!validate_block_fast(curr)) {
            serial_write_str("HEAP ERROR: corrupted free list during alloc at 0x");
            serial_write_hex((uintptr_t)curr);
            serial_write_str("\n");
            PANIC("alloc: corrupted heap");
        }
        if (curr->free && curr->size >= total) break;
        curr = curr->next;
    }

    if (!curr) return NULL;

    split_and_use(curr, total);

    total_allocated += curr->size;
    total_free      -= curr->size;
    if (total_allocated > peak_usage) peak_usage = total_allocated;
    stats.alloc_count++;

    return (void*)((uint8_t*)curr + HEADER_SIZE);
}

// ===========================================
// PUBLIC API
// ===========================================

// alloc: allocate and zero memory
void* alloc(size_t size) {
    if (size == 0) return NULL;

    size_t total = payload_size(size);
    uint64_t flags = allocator_lock();
    void* ptr = alloc_locked(total);
    allocator_unlock(flags);

    if (!ptr) {
        serial_write_str("ALLOC FAILED: size=");
        serial_write_dec(size);
        serial_write_str(", free=");
        serial_write_dec(total_free);
        serial_write_str("\n");
        stats.failed_allocs++;
        return NULL;
    }

    // Zero inside the lock would block IRQs during memset — do it outside
    block_header* b = (block_header*)((uint8_t*)ptr - HEADER_SIZE);
    size_t usable = b->size - FOOTER_SIZE;
    memset(ptr, 0, usable);
    return ptr;
}

// alloc_unzeroed: allocate without zeroing (faster for large buffers you'll overwrite)
void* alloc_unzeroed(size_t size) {
    if (size == 0) return NULL;

    size_t total = payload_size(size);
    uint64_t flags = allocator_lock();
    void* ptr = alloc_locked(total);
    allocator_unlock(flags);

    if (!ptr) {
        stats.failed_allocs++;
        return NULL;
    }
    return ptr;
}

// alloc_array: like calloc — allocates count * elem_size zeroed bytes
void* alloc_array(size_t count, size_t elem_size) {
    if (count == 0 || elem_size == 0) return NULL;
    // Overflow check
    if (elem_size > SIZE_MAX / count) {
        serial_write_str("ALLOC FAILED: alloc_array overflow\n");
        return NULL;
    }
    return alloc(count * elem_size);
}

// alloc_strdup: duplicate a C string into heap memory
char* alloc_strdup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char* copy = (char*)alloc(len);
    if (copy) memcpy(copy, str, len);
    return copy;
}

// alloc_resize: resize an allocation (like realloc)
void* alloc_resize(void* ptr, size_t new_size) {
    if (!ptr)      return alloc(new_size);
    if (!new_size) { free_mem(ptr); return NULL; }

    block_header* b = (block_header*)((uint8_t*)ptr - HEADER_SIZE);

    uint64_t flags = allocator_lock();
    if (!validate_block_full(b, "alloc_resize")) {
        allocator_unlock(flags);
        return NULL;
    }
    size_t old_usable = b->size - FOOTER_SIZE;
    allocator_unlock(flags);

    if (new_size <= old_usable) {
        // Already fits — optionally split off the excess
        return ptr;
    }

    // Grow: allocate new block, copy, free old
    void* new_ptr = alloc(new_size);
    if (!new_ptr) return NULL;  // ptr unchanged on failure

    memcpy(new_ptr, ptr, old_usable);
    free_mem(ptr);
    stats.resize_count++;
    return new_ptr;
}

// free_mem: return memory to the heap
void free_mem(void* ptr) {
    if (!ptr) return;

    if (ptr < heap_start || ptr >= heap_end) {
        serial_write_str("ERROR: free_mem invalid ptr 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        return;
    }

    block_header* b = (block_header*)((uint8_t*)ptr - HEADER_SIZE);

    uint64_t flags = allocator_lock();

    if (!validate_block_full(b, "free_mem")) {
        allocator_unlock(flags);
        return;
    }

    if (b->free) {
        serial_write_str("WARNING: free_mem double-free at 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str("\n");
        allocator_unlock(flags);
        return;
    }

    if (!check_footer(b)) {
        serial_write_str("ERROR: heap overflow detected at 0x");
        serial_write_hex((uintptr_t)ptr);
        serial_write_str(", footer=0x");
        serial_write_hex(*block_footer(b));
        serial_write_str("\n");
        allocator_unlock(flags);
        PANIC("free_mem: heap overflow (buffer overrun)");
    }

    // Scrub freed memory to catch use-after-free bugs
    size_t usable = b->size - FOOTER_SIZE;
    memset(ptr, 0xCC, usable);

    total_allocated -= b->size;
    total_free      += b->size;
    stats.free_count++;

    insert_free_block(b);
    coalesce();

    allocator_unlock(flags);
}

// ===========================================
// ALIGNED ALLOCATION
// ===========================================

static inline uintptr_t align_up(uintptr_t v, size_t a) {
    return (v + a - 1) & ~(uintptr_t)(a - 1);
}

static inline bool is_pow2(size_t x) { return x && !(x & (x - 1)); }

void* kmalloc_aligned(size_t size, size_t alignment) {
    if (size == 0) return NULL;
    if (alignment < ALIGNMENT) alignment = ALIGNMENT;
    if (!is_pow2(alignment)) {
        serial_write_str("ERROR: kmalloc_aligned: alignment not power of two\n");
        return NULL;
    }

    // Allocate extra space so we can shift the returned pointer to be aligned.
    // We store the original pointer just before the aligned pointer.
    size_t total = size + alignment + sizeof(void*);
    void* raw = alloc_unzeroed(total);
    if (!raw) return NULL;

    uintptr_t raw_addr     = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned_addr = align_up(raw_addr, alignment);

    // Store original pointer one slot before the aligned address
    ((void**)aligned_addr)[-1] = raw;

    // Zero the usable portion
    memset((void*)aligned_addr, 0, size);
    return (void*)aligned_addr;
}

void kfree_aligned(void* ptr) {
    if (!ptr) return;
    void* raw = ((void**)ptr)[-1];
    free_mem(raw);
}

// ===========================================
// DIAGNOSTICS
// ===========================================

size_t allocator_used_bytes(void) { return total_allocated; }
size_t allocator_free_bytes(void) { return total_free; }

void allocator_stats(void) {
    serial_write_str("=== Allocator Stats ===\n");
    serial_write_str("Heap:        "); serial_write_hex((uintptr_t)heap_start);
    serial_write_str(" - ");          serial_write_hex((uintptr_t)heap_end);  serial_write_str("\n");
    serial_write_str("Used:        "); serial_write_dec(total_allocated);      serial_write_str(" B\n");
    serial_write_str("Free:        "); serial_write_dec(total_free);           serial_write_str(" B\n");
    serial_write_str("Peak:        "); serial_write_dec(peak_usage);           serial_write_str(" B\n");
    serial_write_str("Allocs:      "); serial_write_dec(stats.alloc_count);    serial_write_str("\n");
    serial_write_str("Frees:       "); serial_write_dec(stats.free_count);     serial_write_str("\n");
    serial_write_str("Resizes:     "); serial_write_dec(stats.resize_count);   serial_write_str("\n");
    serial_write_str("Splits:      "); serial_write_dec(stats.split_count);    serial_write_str("\n");
    serial_write_str("Coalesces:   "); serial_write_dec(stats.coalesce_count); serial_write_str("\n");
    serial_write_str("Failed:      "); serial_write_dec(stats.failed_allocs);  serial_write_str("\n");
    serial_write_str("=======================\n");
}

void allocator_debug(void) {
    serial_write_str("=== Allocator Free List ===\n");
    block_header* curr = free_list_head;
    int count = 0;
    while (curr && count < 64) {
        serial_write_str("  ["); serial_write_dec(count); serial_write_str("] 0x");
        serial_write_hex((uintptr_t)curr);
        serial_write_str(" size="); serial_write_dec(curr->size);
        serial_write_str(" magic="); serial_write_hex(curr->magic);
        serial_write_str("\n");
        curr = curr->next;
        count++;
    }
    if (count == 64) serial_write_str("  ... (truncated)\n");
    serial_write_str("===========================\n");
}

bool allocator_check(void) {
    uint64_t flags = allocator_lock();
    block_header* curr = free_list_head;
    size_t max = heap_size / (HEADER_SIZE + MIN_BLOCK_SIZE) + 1;
    size_t visited = 0;
    bool ok = true;

    while (curr) {
        if (++visited > max) {
            serial_write_str("CHECK FAIL: free list cycle\n");
            ok = false; break;
        }
        if (!validate_block_fast(curr)) {
            serial_write_str("CHECK FAIL: bad block at 0x");
            serial_write_hex((uintptr_t)curr);
            serial_write_str("\n");
            ok = false; break;
        }
        if (!check_footer(curr)) {
            serial_write_str("CHECK FAIL: bad footer at 0x");
            serial_write_hex((uintptr_t)curr);
            serial_write_str("\n");
            ok = false; break;
        }
        curr = curr->next;
    }

    allocator_unlock(flags);
    return ok;
}