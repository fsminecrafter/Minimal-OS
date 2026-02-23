#ifndef MEMORYSCANNER_H
#define MEMORYSCANNER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Memory region types
typedef enum {
    MEM_UNKNOWN = 0,
    MEM_RESERVED,           // Reserved/unusable
    MEM_KERNEL_CODE,        // Kernel executable code
    MEM_KERNEL_DATA,        // Kernel data/BSS
    MEM_KERNEL_STACK,       // Kernel stack
    MEM_PAGE_TABLES,        // Page table structures
    MEM_MMIO_STRUCTURES,    // MMIO tracking arrays
    MEM_HEAP,               // Kernel heap (allocator)
    MEM_PMM_POOL,           // Physical memory manager pool
    MEM_FRAMEBUFFER,        // GPU framebuffer mapping
    MEM_DEVICE_MMIO,        // Device MMIO regions
    MEM_FREE,               // Free/unallocated
    MEM_PROCESS_CODE,       // Process code
    MEM_PROCESS_DATA,       // Process data
    MEM_PROCESS_STACK,      // Process stack
} memory_region_type_t;

// Memory region descriptor
typedef struct {
    uintptr_t start;
    uintptr_t end;
    size_t size;
    memory_region_type_t type;
    const char* name;
    bool is_zeroed;
    uint32_t sample_value;  // First non-zero value found (for debugging)
} memory_region_t;

// Memory scan configuration
typedef struct {
    uintptr_t scan_start;
    uintptr_t scan_end;
    size_t sample_interval;     // How often to sample (every N bytes)
    bool detect_patterns;       // Try to detect patterns in memory
    bool verbose;               // Print detailed info during scan
} memory_scan_config_t;

// Memory statistics
typedef struct {
    size_t total_scanned;
    size_t total_used;
    size_t total_free;
    size_t total_reserved;
    size_t kernel_usage;
    size_t heap_usage;
    size_t pmm_usage;
    size_t device_usage;
    int num_regions;
} memory_stats_t;

// Core scanning functions
void memory_scanner_init(void);
void memory_scan_full(void);
void memory_scan_range(uintptr_t start, uintptr_t end);
void memory_scan_with_config(memory_scan_config_t* config);

// Region detection (automatically identifies memory regions)
void memory_detect_regions(memory_region_t* regions, int* num_regions, int max_regions);

// Pretty printing
void memory_print_map(void);
void memory_print_stats(void);
void memory_print_region(memory_region_t* region);

// Utility functions
const char* memory_type_to_string(memory_region_type_t type);
bool memory_is_zeroed(void* ptr, size_t size);
memory_region_type_t memory_identify_region(uintptr_t address);

// Export known memory regions (filled by scanner or manually)
extern memory_region_t g_known_regions[];
extern int g_num_known_regions;

#endif // MEMORYSCANNER_H