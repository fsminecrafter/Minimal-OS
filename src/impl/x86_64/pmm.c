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
    serial_write_str("alloc_page_zeroed: START\n");
    
    // Check current stack pointer
    uintptr_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    serial_write_str("Current RSP: ");
    serial_write_hex(rsp);
    serial_write_str("\n");
    
    serial_write_str("alloc_page_zeroed: used=");
    serial_write_dec(page_pool_used);
    serial_write_str("/");
    serial_write_dec(page_pool_pages);
    serial_write_str("\n");
    
    if (page_pool_used >= page_pool_pages) {
        PANIC("Out of physical pages in PMM");
    }

    void* page = (void*)(page_pool_base + page_pool_used * PAGE_SIZE);
    serial_write_str("alloc_page_zeroed: page=");
    serial_write_hex((uintptr_t)page);
    serial_write_str("\n");
    
    // Check if we're about to zero the stack!
    uintptr_t page_start = (uintptr_t)page;
    uintptr_t page_end = page_start + PAGE_SIZE;
    
    if (rsp >= page_start && rsp < page_end) {
        serial_write_str("ERROR: Page overlaps with stack! Stack will be destroyed!\n");
        serial_write_str("Stack range: ");
        serial_write_hex(rsp - 0x100);
        serial_write_str(" - ");
        serial_write_hex(rsp + 0x100);
        serial_write_str("\n");
        serial_write_str("Page range: ");
        serial_write_hex(page_start);
        serial_write_str(" - ");
        serial_write_hex(page_end);
        serial_write_str("\n");
        PANIC("Cannot zero page - would destroy stack!");
    }
    
    page_pool_used++;
    
    serial_write_str("alloc_page_zeroed: about to call memset\n");

    // Zero the page using memset
    memset(page, 0, PAGE_SIZE);
    
    serial_write_str("alloc_page_zeroed: memset returned\n");
    serial_write_str("alloc_page_zeroed: DONE\n");

    return page;
}

void free_page(void* page) {
    // Optional: implement free later
    (void)page;
    serial_write_str("Warning: free_page not implemented yet\n");
}