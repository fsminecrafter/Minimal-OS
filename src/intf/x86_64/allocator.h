#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ===========================================
// CORE ALLOCATION
// ===========================================

// Initialize the allocator with a heap region.
void allocator_init(void* memory, size_t size);

// Allocate `size` bytes. Returns zeroed memory or NULL on failure.
void* alloc(size_t size);

// Allocate `size` bytes. NOT zeroed (slightly faster than alloc).
void* alloc_unzeroed(size_t size);

// Allocate and zero `count * size` bytes (like calloc).
void* alloc_array(size_t count, size_t elem_size);

// Resize a previously allocated block.
// If ptr is NULL, behaves like alloc(new_size).
// If new_size is 0, behaves like free_mem(ptr) and returns NULL.
// Returns NULL and leaves ptr unchanged on failure.
void* alloc_resize(void* ptr, size_t new_size);

// Duplicate a string into a heap allocation.
// Returns NULL on failure.
char* alloc_strdup(const char* str);

// Free a previously allocated block. Safe to call with NULL.
void free_mem(void* ptr);

// ===========================================
// ALIGNED ALLOCATION
// ===========================================

// Allocate `size` bytes aligned to `alignment` (must be power of two).
void* kmalloc_aligned(size_t size, size_t alignment);

// Free a block allocated by kmalloc_aligned.
void kfree_aligned(void* ptr);

// ===========================================
// DIAGNOSTICS
// ===========================================

// Print allocator statistics to serial.
void allocator_stats(void);

// Print allocator free-list to serial (for debugging).
void allocator_debug(void);

// Returns true if the heap appears consistent (no obvious corruption).
bool allocator_check(void);

// Returns current bytes allocated (excludes headers).
size_t allocator_used_bytes(void);

// Returns current free bytes.
size_t allocator_free_bytes(void);

#endif // ALLOCATOR_H