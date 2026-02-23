#include "x86_64/memoryscanner.h"
#include "serial.h"
#include "string.h"

#define MAX_MEMORY_REGIONS 64

memory_region_t g_known_regions[MAX_MEMORY_REGIONS];
int g_num_known_regions = 0;

static struct {
    uintptr_t kernel_start;
    uintptr_t kernel_end;
    uintptr_t stack_bottom;  // Stack grows DOWN
    uintptr_t stack_top;
    uintptr_t heap_start;
    uintptr_t heap_end;
    uintptr_t pmm_start;
    uintptr_t pmm_end;
    uintptr_t mmio_arrays_start;
    uintptr_t mmio_arrays_end;
    uintptr_t page_tables_start;
    uintptr_t page_tables_end;
} memory_layout = {0};

const char* memory_type_to_string(memory_region_type_t type) {
    switch (type) {
        case MEM_UNKNOWN:           return "Unknown";
        case MEM_RESERVED:          return "Reserved";
        case MEM_KERNEL_CODE:       return "Kernel Code";
        case MEM_KERNEL_DATA:       return "Kernel Data";
        case MEM_KERNEL_STACK:      return "Kernel Stack";
        case MEM_PAGE_TABLES:       return "Page Tables";
        case MEM_MMIO_STRUCTURES:   return "MMIO Structures";
        case MEM_HEAP:              return "Kernel Heap";
        case MEM_PMM_POOL:          return "PMM Pool";
        case MEM_FRAMEBUFFER:       return "Framebuffer";
        case MEM_DEVICE_MMIO:       return "Device MMIO";
        case MEM_FREE:              return "Free/Unallocated";
        case MEM_PROCESS_CODE:      return "Process Code";
        case MEM_PROCESS_DATA:      return "Process Data";
        case MEM_PROCESS_STACK:     return "Process Stack";
        default:                    return "Invalid";
    }
}

void memory_scanner_init(void) {
    serial_write_str("=== Memory Scanner Initialization ===\n");
    
    // CRITICAL: Get actual addresses from kernel symbols or runtime detection
    // These are NON-OVERLAPPING regions based on your actual memory layout
    
    // Low memory reservation (BIOS, IVT, etc)
    // 0x000000 - 0x100000 (1 MB)
    
    // Kernel loaded at 1MB by GRUB
    // Typical kernel is ~1-2 MB
    memory_layout.kernel_start = 0x100000;    // 1 MB
    memory_layout.kernel_end   = 0x10F000;    // ~60 KB for kernel code+data
    
    // Page tables allocated by PMM (first allocations from 0x1000000)
    // Your earlier output showed: 0x100000, 0x101000
    // But with the fix, PMM starts at 0x1000000, so first tables are there
    memory_layout.page_tables_start = 0x1000000;   // 16 MB (PMM first allocations)
    memory_layout.page_tables_end   = 0x1010000;   // ~64 KB for page tables
    
    // Stack pointer was at 0x115EF0, stack grows down from ~0x116000
    memory_layout.stack_top    = 0x116000;    // Top of stack (high address)
    memory_layout.stack_bottom = 0x110000;    // Bottom of stack (low address, grows down)
    
    // MMIO arrays (from your debug output: 0x117040, 0x118040, 0x318040, 0x318240)
    memory_layout.mmio_arrays_start = 0x117000;
    memory_layout.mmio_arrays_end   = 0x320000;   // Covers all MMIO arrays
    
    // Gap for safety (0x320000 - 0x500000)
    
    // Heap starts at 0x500000 (5 MB) per your fix
    memory_layout.heap_start = 0x500000;
    memory_layout.heap_end   = 0x10000000;   // 256 MB
    
    // PMM pool starts at 0x1000000 (16 MB) but actual free pages start after page tables
    memory_layout.pmm_start = 0x1010000;     // After page tables
    memory_layout.pmm_end   = 0x5000000;     // 80 MB total
    
    serial_write_str("Memory layout initialized (non-overlapping regions)\n");
    serial_write_str("====================================\n\n");
}

bool memory_is_zeroed(void* ptr, size_t size) {
    uint8_t* bytes = (uint8_t*)ptr;
    for (size_t i = 0; i < size; i += 64) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    if (size > 0 && bytes[size - 1] != 0) {
        return false;
    }
    return true;
}

memory_region_type_t memory_identify_region(uintptr_t address) {
    if (address >= memory_layout.kernel_start && address < memory_layout.kernel_end) {
        return MEM_KERNEL_CODE;  // Simplified - includes data too
    }
    if (address >= memory_layout.stack_bottom && address < memory_layout.stack_top) {
        return MEM_KERNEL_STACK;
    }
    if (address >= memory_layout.page_tables_start && address < memory_layout.page_tables_end) {
        return MEM_PAGE_TABLES;
    }
    if (address >= memory_layout.mmio_arrays_start && address < memory_layout.mmio_arrays_end) {
        return MEM_MMIO_STRUCTURES;
    }
    if (address >= memory_layout.heap_start && address < memory_layout.heap_end) {
        return MEM_HEAP;
    }
    if (address >= memory_layout.pmm_start && address < memory_layout.pmm_end) {
        return MEM_PMM_POOL;
    }
    if (address < 0x100000) {
        return MEM_RESERVED;
    }
    if (address >= 0xF0000000ULL) {
        return MEM_DEVICE_MMIO;
    }
    if (address >= 0xFFFFFFA000000000ULL) {
        return MEM_FRAMEBUFFER;
    }
    return MEM_UNKNOWN;
}

void memory_detect_regions(memory_region_t* regions, int* num_regions, int max_regions) {
    int count = 0;
    
    // Reserved low memory
    if (count < max_regions) {
        regions[count].start = 0x0;
        regions[count].end = 0x100000;
        regions[count].size = 0x100000;
        regions[count].type = MEM_RESERVED;
        regions[count].name = "Real Mode / BIOS";
        regions[count].is_zeroed = false;
        count++;
    }
    
    // Kernel (code + data combined for simplicity)
    if (count < max_regions) {
        regions[count].start = memory_layout.kernel_start;
        regions[count].end = memory_layout.kernel_end;
        regions[count].size = memory_layout.kernel_end - memory_layout.kernel_start;
        regions[count].type = MEM_KERNEL_CODE;
        regions[count].name = "Kernel (.text + .data + .bss)";
        regions[count].is_zeroed = false;
        count++;
    }
    
    // Stack (grows downward from stack_top to stack_bottom)
    if (count < max_regions) {
        regions[count].start = memory_layout.stack_bottom;
        regions[count].end = memory_layout.stack_top;
        regions[count].size = memory_layout.stack_top - memory_layout.stack_bottom;
        regions[count].type = MEM_KERNEL_STACK;
        regions[count].name = "Kernel Stack (grows down)";
        regions[count].is_zeroed = false;
        count++;
    }
    
    // MMIO structures
    if (count < max_regions) {
        regions[count].start = memory_layout.mmio_arrays_start;
        regions[count].end = memory_layout.mmio_arrays_end;
        regions[count].size = memory_layout.mmio_arrays_end - memory_layout.mmio_arrays_start;
        regions[count].type = MEM_MMIO_STRUCTURES;
        regions[count].name = "MMIO Page Table Arrays";
        regions[count].is_zeroed = true;
        count++;
    }
    
    // Gap / Free space
    if (count < max_regions && memory_layout.mmio_arrays_end < memory_layout.heap_start) {
        regions[count].start = memory_layout.mmio_arrays_end;
        regions[count].end = memory_layout.heap_start;
        regions[count].size = memory_layout.heap_start - memory_layout.mmio_arrays_end;
        regions[count].type = MEM_FREE;
        regions[count].name = "Unallocated";
        regions[count].is_zeroed = false;
        count++;
    }
    
    // Heap
    if (count < max_regions) {
        regions[count].start = memory_layout.heap_start;
        regions[count].end = memory_layout.heap_end;
        regions[count].size = memory_layout.heap_end - memory_layout.heap_start;
        regions[count].type = MEM_HEAP;
        regions[count].name = "Kernel Heap (Allocator)";
        regions[count].is_zeroed = true;
        count++;
    }
    
    // Page tables (from PMM pool)
    if (count < max_regions) {
        regions[count].start = memory_layout.page_tables_start;
        regions[count].end = memory_layout.page_tables_end;
        regions[count].size = memory_layout.page_tables_end - memory_layout.page_tables_start;
        regions[count].type = MEM_PAGE_TABLES;
        regions[count].name = "Dynamically Allocated Page Tables";
        regions[count].is_zeroed = true;
        count++;
    }
    
    // PMM pool (free pages)
    if (count < max_regions) {
        regions[count].start = memory_layout.pmm_start;
        regions[count].end = memory_layout.pmm_end;
        regions[count].size = memory_layout.pmm_end - memory_layout.pmm_start;
        regions[count].type = MEM_PMM_POOL;
        regions[count].name = "PMM Free Page Pool";
        regions[count].is_zeroed = false;
        count++;
    }
    
    *num_regions = count;
}

void memory_print_region(memory_region_t* region) {
    serial_write_str("  ");
    serial_write_hex(region->start);
    serial_write_str(" - ");
    serial_write_hex(region->end);
    serial_write_str("  |  ");
    
    if (region->size >= 1024 * 1024) {
        serial_write_dec(region->size / (1024 * 1024));
        serial_write_str(" MB");
    } else if (region->size >= 1024) {
        serial_write_dec(region->size / 1024);
        serial_write_str(" KB");
    } else {
        serial_write_dec(region->size);
        serial_write_str(" B");
    }
    serial_write_str("  |  ");
    
    serial_write_str(memory_type_to_string(region->type));
    serial_write_str("  |  ");
    serial_write_str(region->name);
    
    if (region->is_zeroed) {
        serial_write_str("  [ZEROED]");
    }
    
    serial_write_str("\n");
}

void memory_print_map(void) {
    memory_region_t regions[MAX_MEMORY_REGIONS];
    int num_regions = 0;
    
    serial_write_str("\n");
    serial_write_str("========================================\n");
    serial_write_str("         MEMORY MAP                     \n");
    serial_write_str("========================================\n");
    serial_write_str("  Start Address   -   End Address   |  Size   |  Type              |  Description\n");
    serial_write_str("--------------------------------------------------------------------------------------------------------\n");
    
    memory_detect_regions(regions, &num_regions, MAX_MEMORY_REGIONS);
    
    for (int i = 0; i < num_regions; i++) {
        memory_print_region(&regions[i]);
    }
    
    serial_write_str("========================================\n\n");
    
    for (int i = 0; i < num_regions && i < MAX_MEMORY_REGIONS; i++) {
        g_known_regions[i] = regions[i];
    }
    g_num_known_regions = num_regions;
}

void memory_print_stats(void) {
    memory_stats_t stats = {0};
    
    for (int i = 0; i < g_num_known_regions; i++) {
        memory_region_t* region = &g_known_regions[i];
        stats.total_scanned += region->size;
        
        switch (region->type) {
            case MEM_KERNEL_CODE:
            case MEM_KERNEL_DATA:
            case MEM_KERNEL_STACK:
            case MEM_PAGE_TABLES:
            case MEM_MMIO_STRUCTURES:
                stats.kernel_usage += region->size;
                stats.total_used += region->size;
                break;
            case MEM_HEAP:
                stats.heap_usage += region->size;
                stats.total_used += region->size;
                break;
            case MEM_PMM_POOL:
                stats.pmm_usage += region->size;
                stats.total_used += region->size;
                break;
            case MEM_DEVICE_MMIO:
            case MEM_FRAMEBUFFER:
                stats.device_usage += region->size;
                stats.total_used += region->size;
                break;
            case MEM_RESERVED:
                stats.total_reserved += region->size;
                break;
            case MEM_FREE:
                stats.total_free += region->size;
                break;
            default:
                break;
        }
    }
    
    stats.num_regions = g_num_known_regions;
    
    serial_write_str("\n");
    serial_write_str("========================================\n");
    serial_write_str("       MEMORY STATISTICS                \n");
    serial_write_str("========================================\n");
    serial_write_str("Total Memory Regions: ");
    serial_write_dec(stats.num_regions);
    serial_write_str("\n\n");
    serial_write_str("Kernel Usage:  ");
    serial_write_dec(stats.kernel_usage / 1024);
    serial_write_str(" KB (");
    serial_write_dec(stats.kernel_usage / (1024 * 1024));
    serial_write_str(" MB)\n");
    serial_write_str("Heap Usage:    ");
    serial_write_dec(stats.heap_usage / 1024);
    serial_write_str(" KB (");
    serial_write_dec(stats.heap_usage / (1024 * 1024));
    serial_write_str(" MB)\n");
    serial_write_str("PMM Pool:      ");
    serial_write_dec(stats.pmm_usage / 1024);
    serial_write_str(" KB (");
    serial_write_dec(stats.pmm_usage / (1024 * 1024));
    serial_write_str(" MB)\n");
    serial_write_str("Device MMIO:   ");
    serial_write_dec(stats.device_usage / 1024);
    serial_write_str(" KB (");
    serial_write_dec(stats.device_usage / (1024 * 1024));
    serial_write_str(" MB)\n");
    serial_write_str("\nTotal Used:    ");
    serial_write_dec(stats.total_used / 1024);
    serial_write_str(" KB (");
    serial_write_dec(stats.total_used / (1024 * 1024));
    serial_write_str(" MB)\n");
    serial_write_str("Reserved:      ");
    serial_write_dec(stats.total_reserved / 1024);
    serial_write_str(" KB\n");
    serial_write_str("Free:          ");
    serial_write_dec(stats.total_free / 1024);
    serial_write_str(" KB\n");
    serial_write_str("========================================\n\n");
}

void memory_scan_range(uintptr_t start, uintptr_t end) {
    serial_write_str("Scanning memory range: ");
    serial_write_hex(start);
    serial_write_str(" - ");
    serial_write_hex(end);
    serial_write_str("\n");
    
    size_t zeroed_bytes = 0;
    size_t non_zeroed_bytes = 0;
    size_t sample_size = 0x1000;
    
    for (uintptr_t addr = start; addr < end; addr += sample_size) {
        size_t check_size = sample_size;
        if (addr + check_size > end) {
            check_size = end - addr;
        }
        
        if (memory_is_zeroed((void*)addr, check_size)) {
            zeroed_bytes += check_size;
        } else {
            non_zeroed_bytes += check_size;
        }
    }
    
    serial_write_str("  Zeroed: ");
    serial_write_dec(zeroed_bytes / 1024);
    serial_write_str(" KB, Non-zeroed: ");
    serial_write_dec(non_zeroed_bytes / 1024);
    serial_write_str(" KB\n");
}

void memory_scan_full(void) {
    serial_write_str("\n");
    serial_write_str("############################################\n");
    serial_write_str("#     COMPLETE MEMORY SCAN REPORT        #\n");
    serial_write_str("############################################\n\n");
    
    memory_print_map();
    memory_print_stats();
    
    serial_write_str("############################################\n");
    serial_write_str("#          SCAN COMPLETE                 #\n");
    serial_write_str("############################################\n\n");
}

void memory_scan_with_config(memory_scan_config_t* config) {
    if (!config) {
        serial_write_str("Error: NULL config\n");
        return;
    }
    
    serial_write_str("Custom memory scan: ");
    serial_write_hex(config->scan_start);
    serial_write_str(" - ");
    serial_write_hex(config->scan_end);
    serial_write_str("\n");
    
    if (config->verbose) {
        serial_write_str("Verbose mode enabled\n");
        serial_write_str("Sample interval: ");
        serial_write_dec(config->sample_interval);
        serial_write_str(" bytes\n");
    }
    
    memory_scan_range(config->scan_start, config->scan_end);
}