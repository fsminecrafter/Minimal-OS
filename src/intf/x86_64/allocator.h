#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

// Initialize the free-list allocator with a memory region.
// `heap_start` - pointer to the start of free heap memory.
// `heap_size`  - size in bytes of the heap region to manage.
void allocator_init(void* memory, size_t size);

// Allocate a memory block of `size` bytes.
// Returns a pointer to allocated memory or NULL if out of memory.
void* alloc(size_t size);

// Free a previously allocated memory block pointed by `ptr`.
void free_mem(void* ptr);


#endif // ALLOCATOR_H
