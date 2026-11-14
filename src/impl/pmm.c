#include "x86_64/pmm.h"
#include "x86_64/tlb.h"
#include "serial.h"
#include "panic.h"
#include "string.h"
#include "x86_64/allocator.h"

#define PAGE_SIZE 0x1000  // 4 KiB

// ===========================================
// PMM STATE
// Bitmap-based physical page manager.
// Each bit = 1 page. 0 = free, 1 = used.
// ===========================================

static uintptr_t page_pool_base  = 0;
static size_t    page_pool_pages = 0;
static size_t    page_pool_free  = 0;
// Where the next single-page search starts: everything below it is used.
static size_t    page_search_hint = 0;

// One bit per page of the pool. The pool lies inside the identity-mapped
// 4 GiB, so 2^20 pages (128 KiB of bitmap) covers the largest one.
#define PMM_MAX_PAGES (1u << 20)
#define BITMAP_WORDS  ((PMM_MAX_PAGES + 63) / 64)
static uint64_t page_bitmap[BITMAP_WORDS];  // 0 = free, 1 = used

// RAM above 4 GiB (pmm_add_high_region): frames for program memory only.
// Nothing that needs a 32-bit physical address or a contiguous run comes
// from here. Each region has a bitmap of its own on the kernel heap.
#define PMM_HIGH_REGIONS 8
typedef struct {
    uintptr_t base;
    size_t pages;
    size_t free;
    size_t hint;                // word index: every word below it is full
    uint64_t* bitmap;
} pmm_high_t;
static pmm_high_t high_regions[PMM_HIGH_REGIONS];
static size_t high_region_count = 0;
static size_t high_pages_total = 0;
static size_t high_pages_free = 0;

// Interrupt-safe spinlock (same pattern as allocator)
static volatile int pmm_spin = 0;

static inline uint64_t pmm_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&pmm_spin, 1)) { tlb_poll(); }
    return flags;
}

static inline void pmm_unlock(uint64_t flags) {
    __sync_lock_release(&pmm_spin);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static inline void pmm_mark_used(size_t idx) {
    page_bitmap[idx / 64] |= (1ULL << (idx % 64));
}

static inline void pmm_mark_free(size_t idx) {
    page_bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

static inline bool pmm_is_free(size_t idx) {
    return (page_bitmap[idx / 64] & (1ULL << (idx % 64))) == 0;
}

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
    page_pool_free  = page_pool_pages;
    page_search_hint = 0;
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

// The caller holds the lock. PMM_MAX_PAGES when there is no free page.
static size_t pmm_find_one(void) {
    size_t words = (page_pool_pages + 63) / 64;
    for (size_t w = page_search_hint / 64; w < words; w++) {
        uint64_t free_bits = ~page_bitmap[w];
        if (!free_bits) continue;
        size_t idx = w * 64 + (size_t)__builtin_ctzll(free_bits);
        if (idx >= page_pool_pages) break;
        return idx;
    }
    return PMM_MAX_PAGES;
}

// The caller holds the lock. Start of a free run, or PMM_MAX_PAGES.
static size_t pmm_find_run(size_t count) {
    size_t run_start = PMM_MAX_PAGES;
    size_t run_len = 0;
    for (size_t i = page_search_hint; i < page_pool_pages; i++) {
        // Skip a full word at once.
        if ((i % 64) == 0 && page_bitmap[i / 64] == ~0ULL) {
            run_len = 0;
            i += 63;
            continue;
        }
        if (pmm_is_free(i)) {
            if (run_len == 0) run_start = i;
            if (++run_len == count) return run_start;
        } else {
            run_len = 0;
        }
    }
    return PMM_MAX_PAGES;
}

static void* pmm_take(size_t count, bool zero) {
    if (count == 0 || count > page_pool_pages) return NULL;
    uint64_t flags = pmm_lock();

    size_t run_start = PMM_MAX_PAGES;
    if (page_pool_free >= count)
        run_start = count == 1 ? pmm_find_one() : pmm_find_run(count);
    if (run_start == PMM_MAX_PAGES) {
        pmm_unlock(flags);
        return NULL;
    }

    // Never hand out the pages the caller is running on (a stack that was
    // freed while still in use).
    uintptr_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    uintptr_t page_start = page_pool_base + run_start * PAGE_SIZE;
    if (rsp >= page_start && rsp < page_start + count * PAGE_SIZE) {
        pmm_unlock(flags);
        return NULL;
    }

    for (size_t i = run_start; i < run_start + count; i++) {
        pmm_mark_used(i);
    }
    page_pool_free -= count;
    if (run_start == page_search_hint) {
        size_t next = run_start + count;
        while (next < page_pool_pages && !pmm_is_free(next)) next++;
        page_search_hint = next;
    }
    void* pages = pmm_idx_to_ptr(run_start);
    pmm_unlock(flags);

    if (zero) memset(pages, 0, count * PAGE_SIZE);
    return pages;
}

void* pmm_try_alloc_pages_zeroed(size_t count) {
    return pmm_take(count, true);
}

bool pmm_add_high_region(uintptr_t base, size_t size) {
    size_t pages = size / PAGE_SIZE;
    if ((base & (PAGE_SIZE - 1)) || !pages || high_region_count >= PMM_HIGH_REGIONS) return false;
    size_t words = (pages + 63) / 64;
    uint64_t* bitmap = (uint64_t*)alloc(words * sizeof(uint64_t));   // zeroed: all free
    if (!bitmap) return false;
    // Bits past the end of the region count as used.
    if (pages % 64) bitmap[words - 1] = ~0ULL << (pages % 64);

    uint64_t flags = pmm_lock();
    pmm_high_t* r = &high_regions[high_region_count];
    r->base = base;
    r->pages = pages;
    r->free = pages;
    r->hint = 0;
    r->bitmap = bitmap;
    high_pages_total += pages;
    high_pages_free += pages;
    high_region_count++;            // last: readers without the lock see a complete entry
    pmm_unlock(flags);

    serial_write_str("PMM high region: ");
    serial_write_hex(base);
    serial_write_str(" -> ");
    serial_write_hex(base + pages * PAGE_SIZE);
    serial_write_str(", pages: ");
    serial_write_dec(pages);
    serial_write_str("\n");
    return true;
}

static void* pmm_take_high(void) {
    if (!high_pages_free) return NULL;
    uint64_t flags = pmm_lock();
    for (size_t i = 0; i < high_region_count; i++) {
        pmm_high_t* r = &high_regions[i];
        if (!r->free) continue;
        size_t words = (r->pages + 63) / 64;
        for (size_t w = r->hint; w < words; w++) {
            uint64_t free_bits = ~r->bitmap[w];
            if (!free_bits) continue;
            size_t bit = (size_t)__builtin_ctzll(free_bits);
            r->bitmap[w] |= 1ULL << bit;
            r->hint = w;
            r->free--;
            high_pages_free--;
            pmm_unlock(flags);
            return (void*)(r->base + (w * 64 + bit) * PAGE_SIZE);
        }
    }
    pmm_unlock(flags);
    return NULL;
}

// True (and the page is freed) when `page` belongs to a high region.
static bool pmm_free_high(void* page) {
    uintptr_t addr = (uintptr_t)page;
    for (size_t i = 0; i < high_region_count; i++) {
        pmm_high_t* r = &high_regions[i];
        if (addr < r->base || addr >= r->base + r->pages * PAGE_SIZE) continue;
        size_t idx = (addr - r->base) / PAGE_SIZE;
        uint64_t flags = pmm_lock();
        if (!(r->bitmap[idx / 64] & (1ULL << (idx % 64)))) {
            pmm_unlock(flags);
            serial_write_str("WARNING: free_page double-free at 0x");
            serial_write_hex(addr);
            serial_write_str("\n");
            return true;
        }
        r->bitmap[idx / 64] &= ~(1ULL << (idx % 64));
        if (idx / 64 < r->hint) r->hint = idx / 64;
        r->free++;
        high_pages_free++;
        pmm_unlock(flags);
        return true;
    }
    return false;
}

void* pmm_try_alloc_page(void) {
    // Program memory: RAM above 4 GiB first, so the low pool stays free
    // for what must be low.
    void* page = pmm_take_high();
    return page ? page : pmm_take(1, false);
}

void* alloc_page_zeroed(void) {
    void* page = pmm_try_alloc_pages_zeroed(1);
    if (!page) PANIC("Out of physical pages in PMM");
    return page;
}

// Allocate N contiguous pages, all zeroed
void* alloc_pages_zeroed(size_t count) {
    if (count == 0) return NULL;
    void* pages = pmm_try_alloc_pages_zeroed(count);
    if (!pages) PANIC("alloc_pages_zeroed: not enough contiguous pages");
    return pages;
}

static void pmm_free(void* page, bool scrub);

void free_page(void* page) {
    pmm_free(page, true);
}

// For program memory: frames are cleared when they are handed out, and
// scrubbing gigabytes on the way back would stall whoever unmaps them.
void pmm_free_page_unscrubbed(void* page) {
    pmm_free(page, false);
}

static void pmm_free(void* page, bool scrub) {
    if (!page) return;
    if (((uintptr_t)page & (PAGE_SIZE - 1)) == 0 && pmm_free_high(page)) return;

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
    if (scrub) memset(page, 0xCC, PAGE_SIZE);

    pmm_mark_free(idx);
    page_pool_free++;
    if (idx < page_search_hint) page_search_hint = idx;
    pmm_unlock(flags);
}

// Free N contiguous pages (must match the count used in alloc_pages_zeroed)
void free_pages(void* base, size_t count) {
    if (!base || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        free_page((void*)((uintptr_t)base + i * PAGE_SIZE));
    }
}

// ===========================================
// QUERY
// ===========================================

size_t pmm_free_pages(void) {
    return page_pool_free + high_pages_free;
}

size_t pmm_low_free_pages(void) {
    return page_pool_free;
}

size_t pmm_total_pages(void) {
    return page_pool_pages + high_pages_total;
}

size_t pmm_used_pages(void) {
    return pmm_total_pages() - pmm_free_pages();
}

bool pmm_owns(const void* ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    if (addr >= page_pool_base && addr < page_pool_base + page_pool_pages * PAGE_SIZE) return true;
    for (size_t i = 0; i < high_region_count; i++)
        if (addr >= high_regions[i].base &&
            addr < high_regions[i].base + high_regions[i].pages * PAGE_SIZE) return true;
    return false;
}

void pmm_stats(void) {
    size_t used = pmm_used_pages();
    size_t free = pmm_free_pages();
    serial_write_str("=== PMM Stats ===\n");
    serial_write_str("Total pages: "); serial_write_dec(pmm_total_pages()); serial_write_str("\n");
    serial_write_str("Used pages:  "); serial_write_dec(used);            serial_write_str("\n");
    serial_write_str("Free pages:  "); serial_write_dec(free);            serial_write_str("\n");
    serial_write_str("Used KB:     "); serial_write_dec(used * 4);        serial_write_str("\n");
    serial_write_str("Free KB:     "); serial_write_dec(free * 4);        serial_write_str("\n");
    serial_write_str("=================\n");
}
