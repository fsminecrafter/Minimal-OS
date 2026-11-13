#ifndef PMM_H
#define PMM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Initialize the PMM with a physical memory region
// `base` must be 4 KiB aligned
// `size` is the size of the region in bytes (should be multiple of 4 KiB)
void pmm_init(void* base, size_t size);

// Allocate a single 4 KiB zeroed page
void* alloc_page_zeroed(void);

// Try to allocate a contiguous run of zeroed pages without panicking.
// Returns NULL if the requested run is unavailable.
void* pmm_try_alloc_pages_zeroed(size_t count);

// One page for program memory, not cleared, or NULL when every pool is
// empty. It may lie above 4 GiB: use it only through the identity map.
void* pmm_try_alloc_page(void);

// Adds RAM above 4 GiB (already identity mapped) to what
// pmm_try_alloc_page() hands out.
bool pmm_add_high_region(uintptr_t base, size_t size);

// Allocate/free contiguous runs of 4 KiB pages
void* alloc_pages_zeroed(size_t count);
void free_pages(void* base, size_t count);

// Optional: free a page back to PMM
// For now, freeing is optional; pages can be kept forever
void free_page(void* page);
// The same without overwriting the page first (program memory, uvm.c).
void pmm_free_page_unscrubbed(void* page);

// Stats helpers
size_t pmm_free_pages(void);
// Free pages of the pool below 4 GiB only (what the alloc_pages calls use).
size_t pmm_low_free_pages(void);
size_t pmm_used_pages(void);
size_t pmm_total_pages(void);
// True when ptr lies inside the page pool.
bool pmm_owns(const void* ptr);
void pmm_stats(void);

#endif // PMM_H
