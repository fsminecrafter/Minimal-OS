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

// One page, not cleared, or NULL when the pool is empty.
void* pmm_try_alloc_page(void);

// Allocate/free contiguous runs of 4 KiB pages
void* alloc_pages_zeroed(size_t count);
void free_pages(void* base, size_t count);

// Optional: free a page back to PMM
// For now, freeing is optional; pages can be kept forever
void free_page(void* page);

// Stats helpers
size_t pmm_free_pages(void);
size_t pmm_used_pages(void);
size_t pmm_total_pages(void);
// True when ptr lies inside the page pool.
bool pmm_owns(const void* ptr);
void pmm_stats(void);

#endif // PMM_H
