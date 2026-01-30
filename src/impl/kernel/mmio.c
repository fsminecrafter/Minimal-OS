#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "panic.h"
#include "serial.h"
#include "x86_64/pci.h"

extern uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
extern void     pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);

/* Function prototypes - ensure these match exactly */
void mmio_init(void);
void* mmio_alloc_va(size_t size);
bool map_mmio_page(uintptr_t virt_addr, uintptr_t phys_addr, size_t size);
size_t pci_get_bar_size(pci_device_t* dev, int bar_num);
volatile void* pci_map_bar_mmio(pci_device_t* dev, int bar_num);

/* --------------------------------------------------
 * Debug tracking
 * -------------------------------------------------- */

uintptr_t savedfirstvaddr = 0;
uintptr_t savedfirstpaddr = 0;
uintptr_t savedlastvaddr  = 0;
uintptr_t savedlastpaddr  = 0;
static size_t map_index   = 0;

/* --------------------------------------------------
 * PCI BAR sizing
 * -------------------------------------------------- */

size_t pci_get_bar_size(pci_device_t* dev, int bar_num) {
    if (!dev || bar_num < 0 || bar_num >= PCI_NUM_BARS)
        return 0;

    uint8_t bus      = dev->bus;
    uint8_t device   = dev->device;
    uint8_t function = dev->function;
    uint8_t offset   = 0x10 + bar_num * 4;

    uint32_t original = pci_config_read32(bus, device, function, offset);

    pci_config_write32(bus, device, function, offset, 0xFFFFFFFF);
    uint32_t size_mask = pci_config_read32(bus, device, function, offset);

    pci_config_write32(bus, device, function, offset, original);

    if (original & 0x1) {
        size_mask &= ~0x3;     // I/O BAR
    } else {
        size_mask &= ~0xF;     // MMIO BAR
    }

    if (size_mask == 0)
        return 0;

    return (~size_mask) + 1;
}

/* --------------------------------------------------
 * Paging constants
 * -------------------------------------------------- */

#define PAGE_PRESENT        0x001
#define PAGE_WRITABLE       0x002
#define PAGE_CACHE_DISABLE  0x010

#define MMIO_PAGE_SIZE  0x1000
#define MMIO_VA_ALIGN   0x1000

/* IMPORTANT: Use the MMIO range set up by the bootloader at PML4[511] */
/* PML4[511] covers virtual addresses 0xFFFFFF8000000000 to 0xFFFFFFFFFFFFFFFF */
/* We'll use a range well within this that doesn't conflict with anything */
#define MMIO_VA_BASE  0xFFFFFFA000000000ULL
#define MMIO_VA_END   0xFFFFFFAFFFFFFFFFULL

#define MMIO_PAGE_SIZE_4K 0x1000
#define MMIO_PAGE_SIZE_2M 0x200000
#define PAGE_SIZE_FLAG    0x080   // for 2 MiB pages in PTE


/* --------------------------------------------------
 * Page tables
 * -------------------------------------------------- */

extern uint64_t page_table_l3_mmio[512];

/* One L2 per L3 */
static uint64_t* page_table_l2_mmio[512];
static uint64_t* page_table_l1_mmio[512][512];

static bool l2_installed[512];
static bool l1_installed[512][512];

/* --------------------------------------------------
 * VA allocator
 * -------------------------------------------------- */

static inline uintptr_t align_down(uintptr_t v, uintptr_t a) {
    return v & ~(a - 1);
}

static inline uintptr_t align_up(uintptr_t v, uintptr_t a) {
    return (v + a - 1) & ~(a - 1);
}

static uintptr_t mmio_next_va = MMIO_VA_BASE;
static bool mmio_initialized = false;

void mmio_init(void) {
    serial_write_str("Initializing MMIO subsystem...\n");
    
    // Show where the arrays are located
    serial_write_str("Array locations:\n");
    serial_write_str("  page_table_l2_mmio: ");
    serial_write_hex((uintptr_t)&page_table_l2_mmio);
    serial_write_str("\n");
    serial_write_str("  page_table_l1_mmio: ");
    serial_write_hex((uintptr_t)&page_table_l1_mmio);
    serial_write_str("\n");
    serial_write_str("  l2_installed: ");
    serial_write_hex((uintptr_t)&l2_installed);
    serial_write_str("\n");
    serial_write_str("  l1_installed: ");
    serial_write_hex((uintptr_t)&l1_installed);
    serial_write_str("\n");
    
    // Explicitly zero all static arrays (bare-metal kernel, no C runtime)
    serial_write_str("Zeroing MMIO page table arrays...\n");
    for (int i = 0; i < 512; i++) {
        if (i % 128 == 0) {
            serial_write_str("  Zeroing index ");
            serial_write_dec(i);
            serial_write_str("/512\n");
        }
        page_table_l2_mmio[i] = NULL;
        l2_installed[i] = false;
        for (int j = 0; j < 512; j++) {
            page_table_l1_mmio[i][j] = NULL;
            l1_installed[i][j] = false;
        }
    }
    serial_write_str("MMIO arrays zeroed\n");
    
    mmio_next_va = MMIO_VA_BASE;
    mmio_initialized = true;
    
    serial_write_str("MMIO VA range: ");
    serial_write_hex(MMIO_VA_BASE);
    serial_write_str(" - ");
    serial_write_hex(MMIO_VA_END);
    serial_write_str("\n");
    
    // The page tables are already installed by the assembly boot code
    // page_table_l3_mmio is at PML4[511] and page_table_l2_mmio is at L3[0]
    // We just need to verify it's there
    
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* pml4 = (uint64_t*)(cr3 & ~0xFFFULL);
    
    // The MMIO VA base 0xFFFF900000000000 maps to PML4 index 498
    // But your assembly installs it at index 511, so let's check what address that is
    // PML4[511] = virtual addresses 0xFFFF800000000000 to 0xFFFFFFFFFFFFFFFF
    // But we want to use a specific range for MMIO
    
    serial_write_str("PML4 entry for MMIO (index 511): ");
    serial_write_hex(pml4[511]);
    serial_write_str("\n");
    
    // Note: The assembly sets up PML4[511] with page_table_l3_mmio
    // This covers VAs from 0xFFFF800000000000 and up
    // Our MMIO_VA_BASE (0xFFFF900000000000) falls within this range
    
    serial_write_str("MMIO page tables ready (initialized by bootloader)\n");
}

void* mmio_alloc_va(size_t size) {
    if (!mmio_initialized) {
        serial_write_str("ERROR: mmio_alloc_va called before mmio_init!\n");
        return NULL;
    }
    
    if (size == 0) {
        serial_write_str("ERROR: mmio_alloc_va called with size 0\n");
        return NULL;
    }
    
    mmio_next_va = align_up(mmio_next_va, MMIO_VA_ALIGN);
    
    if (mmio_next_va + size > MMIO_VA_END) {
        serial_write_str("ERROR: MMIO VA space exhausted\n");
        return NULL;
    }
    
    void* base = (void*)mmio_next_va;
    mmio_next_va += size;
    
    serial_write_str("mmio_alloc_va: allocated ");
    serial_write_hex(size);
    serial_write_str(" bytes at ");
    serial_write_hex((uintptr_t)base);
    serial_write_str("\n");
    
    return base;
}

/* --------------------------------------------------
 * MMIO page mapping
 * -------------------------------------------------- */
bool map_mmio_page(uintptr_t virt_addr, uintptr_t phys_addr, size_t size) {
    serial_write_str("map_mmio_page: virt=");
    serial_write_hex(virt_addr);
    serial_write_str(" phys=");
    serial_write_hex(phys_addr);
    serial_write_str(" size=");
    serial_write_hex(size);
    serial_write_str("\n");
    
    if (virt_addr == 0) {
        serial_write_str("ERROR: map_mmio_page called with NULL virtual address\n");
        return false;
    }
    
    // Map the entire region by looping
    uintptr_t current_virt = virt_addr;
    uintptr_t current_phys = phys_addr;
    size_t remaining = size;
    int pages_mapped = 0;
    
    while (remaining > 0) {
        size_t l3_index = (current_virt >> 30) & 0x1FF;
        size_t l2_index = (current_virt >> 21) & 0x1FF;

        // Only print detailed output for first page to reduce spam
        bool verbose = (pages_mapped == 0);
        
        if (verbose) {
            serial_write_str("  L3 index: ");
            serial_write_hex(l3_index);
            serial_write_str(" L2 index: ");
            serial_write_hex(l2_index);
            serial_write_str("\n");
        }

        // Allocate L2 if missing
        if (!page_table_l2_mmio[l3_index]) {
            serial_write_str("  Allocating L2 table for L3 index ");
            serial_write_hex(l3_index);
            serial_write_str("\n");
            
            page_table_l2_mmio[l3_index] = alloc_page_zeroed();
            if (!page_table_l2_mmio[l3_index]) {
                serial_write_str("ERROR: Failed to allocate L2 page table\n");
                return false;
            }
            
            page_table_l3_mmio[l3_index] =
                ((uintptr_t)page_table_l2_mmio[l3_index]) |
                PAGE_PRESENT | PAGE_WRITABLE;
                
            serial_write_str("  L2 table allocated at ");
            serial_write_hex((uintptr_t)page_table_l2_mmio[l3_index]);
            serial_write_str("\n");
        }

        uint64_t* l2_table = page_table_l2_mmio[l3_index];

        // Try to use 2 MiB page if aligned and enough space remains
        if ((current_virt & (MMIO_PAGE_SIZE_2M - 1)) == 0 &&
            (current_phys & (MMIO_PAGE_SIZE_2M - 1)) == 0 &&
            remaining >= MMIO_PAGE_SIZE_2M) {

            if (verbose) {
                serial_write_str("  Mapping as 2 MiB pages...\n");
            }
            
            l2_table[l2_index] =
                (current_phys & ~(MMIO_PAGE_SIZE_2M - 1)) |
                PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE | PAGE_SIZE_FLAG;

            if (verbose) {
                serial_write_str("  L2[");
                serial_write_hex(l2_index);
                serial_write_str("] = ");
                serial_write_hex(l2_table[l2_index]);
                serial_write_str("\n");
            }
            
            // Flush TLB for this address
            __asm__ volatile("invlpg (%0)" : : "r"(current_virt) : "memory");
            
            current_virt += MMIO_PAGE_SIZE_2M;
            current_phys += MMIO_PAGE_SIZE_2M;
            remaining -= MMIO_PAGE_SIZE_2M;
            pages_mapped++;
            
        } else {
            // Map as 4 KiB page
            size_t l1_index = (current_virt >> 12) & 0x1FF;

            if (verbose) {
                serial_write_str("  Mapping as 4 KiB page, L1 index: ");
                serial_write_hex(l1_index);
                serial_write_str("\n");
            }

            if (!page_table_l1_mmio[l3_index][l2_index]) {
                serial_write_str("  Allocating L1 table...\n");
                
                void* new_l1 = alloc_page_zeroed();
                
                serial_write_str("  Back in mmio.c after alloc_page_zeroed\n");
                
                if (!new_l1) {
                    serial_write_str("ERROR: alloc_page_zeroed returned NULL\n");
                    return false;
                }
                
                serial_write_str("  L1 table allocated and zeroed at ");
                serial_write_hex((uintptr_t)new_l1);
                serial_write_str("\n");
                
                uint64_t* l1_ptr = (uint64_t*)new_l1;
                page_table_l1_mmio[l3_index][l2_index] = l1_ptr;
                
                l2_table[l2_index] =
                    ((uintptr_t)l1_ptr) |
                    PAGE_PRESENT | PAGE_WRITABLE;
                    
                serial_write_str("  L1 table allocated successfully\n");
            }

            uint64_t* l1_array = page_table_l1_mmio[l3_index][l2_index];
            
            if (!l1_array) {
                serial_write_str("ERROR: L1 array is NULL!\n");
                return false;
            }
            
            l1_array[l1_index] =
                (current_phys & ~(MMIO_PAGE_SIZE_4K - 1)) |
                PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE;

            if (verbose) {
                serial_write_str("  L1[");
                serial_write_hex(l1_index);
                serial_write_str("] = ");
                serial_write_hex(l1_array[l1_index]);
                serial_write_str("\n");
            }
            
            // Flush TLB for this address
            __asm__ volatile("invlpg (%0)" : : "r"(current_virt) : "memory");

            current_virt += MMIO_PAGE_SIZE_4K;
            current_phys += MMIO_PAGE_SIZE_4K;
            remaining -= MMIO_PAGE_SIZE_4K;
            pages_mapped++;
        }
    }
    
    serial_write_str("  Mapped ");
    serial_write_dec(pages_mapped);
    serial_write_str(" pages successfully\n");
    return true;
}

/* --------------------------------------------------
 * PCI BAR MMIO mapping
 * -------------------------------------------------- */
volatile void* pci_map_bar_mmio(pci_device_t* dev, int bar_num) {
    if (!dev || bar_num < 0 || bar_num >= PCI_NUM_BARS)
        PANIC("pci_map_bar_mmio: invalid BAR");

    if (dev->bar_type[bar_num] != PCI_BAR_MEM)
        PANIC("pci_map_bar_mmio: BAR not MMIO");

    uintptr_t phys_addr = dev->bar[bar_num] & ~0xFULL;
    size_t bar_size = pci_get_bar_size(dev, bar_num);

    if (bar_size == 0)
        PANIC("pci_map_bar_mmio: BAR size zero");

    uintptr_t phys_base = phys_addr & ~(MMIO_PAGE_SIZE_4K - 1);
    uintptr_t offset = phys_addr - phys_base;
    size_t total_size = offset + bar_size;

    uintptr_t virt_base = (uintptr_t)mmio_alloc_va(total_size);

    // Map in chunks of 2 MiB if possible, otherwise 4 KiB
    size_t remaining = total_size;
    uintptr_t vaddr = virt_base;
    uintptr_t paddr = phys_base;

    while (remaining > 0) {
        size_t map_size = MMIO_PAGE_SIZE_2M;
        if (remaining < MMIO_PAGE_SIZE_2M) map_size = MMIO_PAGE_SIZE_4K;

        map_mmio_page(vaddr, paddr, map_size);

        vaddr += map_size;
        paddr += map_size;
        remaining -= map_size;
    }

    serial_write_str("BAR mapped: ");
    serial_write_hex(bar_size);
    serial_write_str(" bytes\n");

    return (volatile void*)(virt_base + offset);
}