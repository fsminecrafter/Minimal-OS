#include "x86_64/pmm.h"
#include "serial.h"
#include "panic.h"
#include "string.h"

#define PAGE_SIZE 0x1000  // 4 KiB
#define PMM_LEGACY_CONTIG_ALLOC 0

// ===========================================
// PMM STATE
// Bitmap-based physical page manager.
// Each bit = 1 page. 0 = free, 1 = used.
// ===========================================

static uintptr_t page_pool_base  = 0;
static size_t    page_pool_pages = 0;

// Bitmap: one bit per page. Max 16384 pages (64MB) = 2KB of bitmap.
#define PMM_MAX_PAGES 16384
#define BITMAP_WORDS  ((PMM_MAX_PAGES + 63) / 64)
static uint64_t page_bitmap[BITMAP_WORDS];  // 0 = free, 1 = used

// Interrupt-safe spinlock (same pattern as allocator)
static volatile int pmm_spin = 0;

static inline uint64_t pmm_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&pmm_spin, 1)) { /* spin */ }
    return flags;
}

static inline void pmm_unlock(uint64_t flags) {
    __sync_lock_release(&pmm_spin);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// Mark a page index as used
static inline void pmm_mark_used(size_t idx) {
    page_bitmap[idx / 64] |= (1ULL << (idx % 64));
}

// Mark a page index as free
static inline void pmm_mark_free(size_t idx) {
    page_bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

// Check if a page index is free
static inline bool pmm_is_free(size_t idx) {
    return (page_bitmap[idx / 64] & (1ULL << (idx % 64))) == 0;
}

// Convert page index to pointer
static inline void* pmm_idx_to_ptr(size_t idx) {
    return (void*)(page_pool_base + idx * PAGE_SIZE);
}

// Convert pointer to page index. Returns PMM_MAX_PAGES on invalid pointer.
static inline size_t pmm_ptr_to_idx(void* ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < page_pool_base) return PMM_MAX_PAGES;
    size_t idx = (addr - page_pool_base) / PAGE_SIZE;
    if (idx >= page_pool_pages) return PMM_MAX_PAGES;
    if ((addr - page_pool_base) % PAGE_SIZE != 0) return PMM_MAX_PAGES;
    return idx;
}

// ===========================================
// PMM INIT
// ===========================================

void pmm_init(void* base, size_t size) {
    if (((uintptr_t)base) & (PAGE_SIZE - 1)) {
        PANIC("PMM base not 4 KiB aligned");
    }
    if (size / PAGE_SIZE > PMM_MAX_PAGES) {
        PANIC("PMM region too large for bitmap");
    }

    page_pool_base  = (uintptr_t)base;
    page_pool_pages = size / PAGE_SIZE;
    pmm_spin = 0;

    // Clear bitmap — all pages start free
    for (size_t i = 0; i < BITMAP_WORDS; i++) {
        page_bitmap[i] = 0;
    }

    serial_write_str("PMM initialized: ");
    serial_write_hex(page_pool_base);
    serial_write_str(" -> ");
    serial_write_hex(page_pool_base + page_pool_pages * PAGE_SIZE);
    serial_write_str(", pages: ");
    serial_write_dec(page_pool_pages);
    serial_write_str("\n");
}

// ===========================================
// ALLOC / FREE PAGE
// ===========================================

void* alloc_page_zeroed(void) {
    uint64_t flags = pmm_lock();

    // Find first free page using bitmap
    size_t found = PMM_MAX_PAGES;
    for (size_t i = 0; i < page_pool_pages; i++) {
        if (pmm_is_free(i)) {
            found = i;
            break;
        }
    }

    if (found == PMM_MAX_PAGES) {
        pmm_unlock(flags);
        PANIC("Out of physical pages in PMM");
    }

    pmm_mark_used(found);
    void* page = pmm_idx_to_ptr(found);

    // Check we're not about to zero our own stack
    uintptr_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    uintptr_t page_start = (uintptr_t)page;
    uintptr_t page_end   = page_start + PAGE_SIZE;
    if (rsp >= page_start && rsp < page_end) {
        pmm_mark_free(found);  // undo the allocation
        pmm_unlock(flags);
        PANIC("alloc_page_zeroed: page overlaps with stack");
    }

    pmm_unlock(flags);

    // Zero outside the lock — safe since we own the page now
    memset(page, 0, PAGE_SIZE);

    return page;
}

#if !PMM_LEGACY_CONTIG_ALLOC
// Allocate N contiguous pages, all zeroed
void* alloc_pages_zeroed(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return alloc_page_zeroed();

    uint64_t flags = pmm_lock();

    // Find a contiguous run of `count` free pages
    size_t run_start = PMM_MAX_PAGES;
    size_t run_len = 0;
    for (size_t i = 0; i < page_pool_pages; i++) {
        if (pmm_is_free(i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= count) break;
        } else {
            run_len = 0;
        }
    }

    if (run_len < count) {
        pmm_unlock(flags);
        PANIC("alloc_pages_zeroed: not enough contiguous pages");
    }

    for (size_t i = run_start; i < run_start + count; i++) {
        pmm_mark_used(i);
    }

    void* base = pmm_idx_to_ptr(run_start);
    pmm_unlock(flags);

    memset(base, 0, count * PAGE_SIZE);
    return base;
}

void free_page(void* page) {
    if (!page) return;

    size_t idx = pmm_ptr_to_idx(page);
    if (idx == PMM_MAX_PAGES) {
        serial_write_str("ERROR: free_page of invalid pointer 0x");
        serial_write_hex((uintptr_t)page);
        serial_write_str("\n");
        return;
    }

    uint64_t flags = pmm_lock();

    if (pmm_is_free(idx)) {
        pmm_unlock(flags);
        serial_write_str("WARNING: free_page double-free at 0x");
        serial_write_hex((uintptr_t)page);
        serial_write_str("\n");
        return;
    }

    // Scrub page before returning to pool (catch use-after-free bugs)
    memset(page, 0xCC, PAGE_SIZE);

    pmm_mark_free(idx);
    pmm_unlock(flags);
}

// Free N contiguous pages (must match the count used in alloc_pages_zeroed)
void free_pages(void* base, size_t count) {
    if (!base || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        free_page((void*)((uintptr_t)base + i * PAGE_SIZE));
    }
}
#endif

#if PMM_LEGACY_CONTIG_ALLOC
/**
 * Allocate multiple contiguous pages (zeroed)
 *
 * This is CRITICAL for DMA buffers that need to be physically contiguous
 *
 * @param num_pages Number of 4KB pages to allocate
 * @return Pointer to first page, or NULL on failure
 */
void* alloc_pages_zeroed(uint32_t num_pages) {
    if (num_pages == 0) return NULL;

    // For single page, use existing function
    if (num_pages == 1) {
        return alloc_page_zeroed();
    }

    // Find contiguous run of free pages
    void* first_page = NULL;
    uint32_t found_count = 0;

    // Simple linear search (you may want to optimize this)
    // This assumes your PMM tracks pages somehow
    // Adjust based on your actual PMM implementation

    extern uint32_t g_total_pages;  // Adjust to your PMM globals
    extern bool g_page_allocated[];

    for (uint32_t i = 0; i < g_total_pages && found_count < num_pages; i++) {
        if (!g_page_allocated[i]) {
            if (found_count == 0) {
                // Start of potential run
                first_page = (void*)(uintptr_t)(i * 0x1000);
            }
            found_count++;

            if (found_count == num_pages) {
                // Found enough contiguous pages!
                // Mark them all as allocated
                for (uint32_t j = 0; j < num_pages; j++) {
                    g_page_allocated[i - num_pages + 1 + j] = true;
                }

                // Zero all pages
                memset(first_page, 0, num_pages * 0x1000);

                serial_write_str("PMM: Allocated ");
                serial_write_dec(num_pages);
                serial_write_str(" contiguous pages at 0x");
                serial_write_hex((uintptr_t)first_page);
                serial_write_str("\n");

                return first_page;
            }
        } else {
            // Reset search
            found_count = 0;
            first_page = NULL;
        }
    }

    serial_write_str("PMM: Failed to allocate ");
    serial_write_dec(num_pages);
    serial_write_str(" contiguous pages\n");

    return NULL;
}

/**
 * Free multiple contiguous pages
 *
 * @param ptr Pointer to first page
 * @param num_pages Number of pages to free
 */
void free_pages(void* ptr, uint32_t num_pages) {
    if (!ptr || num_pages == 0) return;

    uintptr_t page_addr = (uintptr_t)ptr;

    // Verify alignment
    if (page_addr & 0xFFF) {
        serial_write_str("PMM: free_pages called with unaligned pointer\n");
        return;
    }

    extern bool g_page_allocated[];

    // Free each page
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t page_index = (page_addr / 0x1000) + i;
        g_page_allocated[page_index] = false;
    }

    serial_write_str("PMM: Freed ");
    serial_write_dec(num_pages);
    serial_write_str(" pages at 0x");
    serial_write_hex(page_addr);
    serial_write_str("\n");
}
#endif

// ===========================================
// QUERY
// ===========================================

size_t pmm_free_pages(void) {
    uint64_t flags = pmm_lock();
    size_t free = 0;
    for (size_t i = 0; i < page_pool_pages; i++) {
        if (pmm_is_free(i)) free++;
    }
    pmm_unlock(flags);
    return free;
}

size_t pmm_used_pages(void) {
    return page_pool_pages - pmm_free_pages();
}

void pmm_stats(void) {
    size_t used = pmm_used_pages();
    size_t free = page_pool_pages - used;
    serial_write_str("=== PMM Stats ===\n");
    serial_write_str("Total pages: "); serial_write_dec(page_pool_pages); serial_write_str("\n");
    serial_write_str("Used pages:  "); serial_write_dec(used);            serial_write_str("\n");
    serial_write_str("Free pages:  "); serial_write_dec(free);            serial_write_str("\n");
    serial_write_str("Used KB:     "); serial_write_dec(used * 4);        serial_write_str("\n");
    serial_write_str("Free KB:     "); serial_write_dec(free * 4);        serial_write_str("\n");
    serial_write_str("=================\n");
}

/*
 * ALTERNATIVE SIMPLER VERSION
 * If your PMM doesn't track individual page allocation,
 * you can use this simplified version:
 */
 
void* alloc_pages_zeroed_simple(uint32_t num_pages) {
    if (num_pages == 0) return NULL;
    
    // Allocate pages one at a time and hope they're contiguous
    // (This is NOT guaranteed to work for large allocations!)
    void* first = alloc_page_zeroed();
    if (!first) return NULL;
    
    void* current = first;
    
    for (uint32_t i = 1; i < num_pages; i++) {
        void* next = alloc_page_zeroed();
        if (!next) {
            // Failed to allocate - free what we got so far
            for (uint32_t j = 0; j < i; j++) {
                free_page((uint8_t*)first + (j * 0x1000));
            }
            return NULL;
        }
        
        // Check if contiguous
        if ((uintptr_t)next != (uintptr_t)current + 0x1000) {
            serial_write_str("PMM WARNING: Non-contiguous pages allocated!\n");
            serial_write_str("  This may cause DMA issues!\n");
        }
        
        current = next;
    }
    
    return first;
}
 
void free_pages_simple(void* ptr, uint32_t num_pages) {
    if (!ptr || num_pages == 0) return;
    
    for (uint32_t i = 0; i < num_pages; i++) {
        free_page((uint8_t*)ptr + (i * 0x1000));
    }
}
