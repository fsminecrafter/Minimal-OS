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

// Optional: free a page back to PMM
// For now, freeing is optional; pages can be kept forever
void free_page(void* page);

#endif // PMM_H
