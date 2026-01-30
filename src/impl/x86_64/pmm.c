#include "x86_64/pmm.h"
#include "serial.h"
#include "panic.h"
#include "string.h"

#define PAGE_SIZE 0x1000  // 4 KiB

static uintptr_t page_pool_base = 0;
static size_t    page_pool_pages = 0;
static size_t    page_pool_used = 0;

void pmm_init(void* base, size_t size) {
    if (((uintptr_t)base) & (PAGE_SIZE - 1)) {
        PANIC("PMM base not 4 KiB aligned");
    }

    page_pool_base  = (uintptr_t)base;
    page_pool_pages = size / PAGE_SIZE;
    page_pool_used  = 0;

    serial_write_str("PMM initialized: ");
    serial_write_hex(page_pool_base);
    serial_write_str(" -> ");
    serial_write_hex(page_pool_base + page_pool_pages * PAGE_SIZE);
    serial_write_str(", pages: ");
    serial_write_dec(page_pool_pages);
    serial_write_str("\n");
}

void* alloc_page_zeroed(void) {
    serial_write_str("alloc_page_zeroed: entering, used=");
    serial_write_dec(page_pool_used);
    serial_write_str("/");
    serial_write_dec(page_pool_pages);
    serial_write_str("\n");
    
    if (page_pool_used >= page_pool_pages) {
        PANIC("Out of physical pages in PMM");
    }

    void* page = (void*)(page_pool_base + page_pool_used * PAGE_SIZE);
    serial_write_str("alloc_page_zeroed: allocated page at ");
    serial_write_hex((uintptr_t)page);
    serial_write_str("\n");
    
    page_pool_used++;

    // Zero the page using memset
    memset(page, 0, PAGE_SIZE);
    
    serial_write_str("alloc_page_zeroed: zeroed page, returning ");
    serial_write_hex((uintptr_t)page);
    serial_write_str("\n");

    return page;
}

void free_page(void* page) {
    // Optional: implement free later
    (void)page;
    serial_write_str("Warning: free_page not implemented yet\n");
}