#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "graphics.h"
#include "serial.h"
#include "x86_64/allocator.h"
#include "x86_64/memory_test.h"
#include "x86_64/pmm.h"

#define MEMORY_TEST_CHUNK_PAGES 256
#define PAGE_BYTES 4096

typedef struct {
    uint64_t* base;
    size_t pages;
} memory_test_run_t;

static void memory_test_report(const char* message) {
    serial_write_str(message);
    serial_write_str("\n");
    if (graphics_get_width()) {
        graphics_write_textr(message);
        graphics_write_textr("\n");
    }
}

static void memory_test_halt(void) __attribute__((__noreturn__));

static void memory_test_halt(void) {
    for (;;) __asm__ volatile("cli; hlt" ::: "memory");
}

static void memory_test_mismatch(uintptr_t address, uint64_t expected,
                                 uint64_t actual) {
    serial_write_str("Memory test mismatch at ");
    serial_write_hex(address);
    serial_write_str(" expected=");
    serial_write_hex(expected);
    serial_write_str(" actual=");
    serial_write_hex(actual);
    serial_write_str("\n");
    if (graphics_get_width()) {
        graphics_write_textr("FAIL at ");
        graphics_write_textr_hex(address);
        graphics_write_textr(" expected ");
        graphics_write_textr_hex(expected);
        graphics_write_textr(" actual ");
        graphics_write_textr_hex(actual);
        graphics_write_textr("\n");
    }
}

static uint64_t memory_test_value(uint64_t* address, uint64_t pattern,
                                  bool address_pattern, bool invert) {
    uint64_t value = address_pattern ? ((uint64_t)(uintptr_t)address ^ pattern)
                                     : pattern;
    return invert ? ~value : value;
}

static bool memory_test_pattern(memory_test_run_t* runs, size_t run_count,
                                uint64_t pattern, bool address_pattern,
                                bool invert) {
    for (size_t run = 0; run < run_count; run++) {
        size_t word_count = runs[run].pages * PAGE_BYTES / sizeof(uint64_t);
        volatile uint64_t* words = runs[run].base;
        for (size_t i = 0; i < word_count; i++) {
            words[i] = memory_test_value((uint64_t*)&words[i], pattern,
                                         address_pattern, invert);
        }
    }
    __asm__ volatile("mfence" ::: "memory");

    for (size_t run = 0; run < run_count; run++) {
        size_t word_count = runs[run].pages * PAGE_BYTES / sizeof(uint64_t);
        volatile uint64_t* words = runs[run].base;
        for (size_t i = 0; i < word_count; i++) {
            uint64_t expected = memory_test_value((uint64_t*)&words[i], pattern,
                                                  address_pattern, invert);
            uint64_t actual = words[i];
            if (actual != expected) {
                memory_test_mismatch((uintptr_t)&words[i], expected, actual);
                return false;
            }
        }
    }
    return true;
}

void memory_test_run(void) {
    __asm__ volatile("cli" ::: "memory");

    uint32_t width = graphics_get_width();
    uint32_t height = graphics_get_height();
    if (width >= 8 && height >= 8) {
        graphics_set_resolution(width / 8, height / 8);
        graphics_terminal_set_color(COLOR_WHITE, COLOR_BLACK);
        graphics_terminal_clear();
    }

    memory_test_report("Minimal-OS x64 PMM Memory Tester");
    size_t page_count = pmm_free_pages();
    if (page_count == 0) {
        memory_test_report("FAIL: no free PMM pages to test");
        memory_test_halt();
    }

    memory_test_run_t* runs = alloc_array(page_count, sizeof(*runs));
    if (!runs) {
        memory_test_report("FAIL: unable to allocate test bookkeeping");
        memory_test_halt();
    }

    size_t run_count = 0;
    size_t pages_remaining = page_count;
    while (pages_remaining) {
        size_t requested = pages_remaining < MEMORY_TEST_CHUNK_PAGES
            ? pages_remaining : MEMORY_TEST_CHUNK_PAGES;
        void* memory = NULL;
        while (requested && !memory) {
            memory = pmm_try_alloc_pages_zeroed(requested);
            if (!memory) requested /= 2;
        }
        if (!memory) {
            memory_test_report("FAIL: unable to reserve all free PMM pages");
            memory_test_halt();
        }
        runs[run_count++] = (memory_test_run_t){
            .base = (uint64_t*)memory,
            .pages = requested,
        };
        pages_remaining -= requested;
    }

    serial_write_str("Testing ");
    serial_write_dec(page_count);
    serial_write_str(" free PMM pages (bytes=");
    serial_write_dec(page_count * PAGE_BYTES);
    serial_write_str(")\n");
    if (graphics_get_width()) {
        graphics_write_textr("Testing free PMM pages: ");
        graphics_write_textr_udec(page_count);
        graphics_write_textr("\n");
    }

    static const uint64_t patterns[] = {
        0x0000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL,
        0xAAAAAAAAAAAAAAAAULL,
        0x5555555555555555ULL,
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        memory_test_report("Testing fixed data pattern");
        if (!memory_test_pattern(runs, run_count, patterns[i], false, false)) {
            memory_test_halt();
        }
    }

    memory_test_report("Testing address-dependent pattern");
    if (!memory_test_pattern(runs, run_count, 0xA55AA55AA55AA55AULL,
                             true, false) ||
        !memory_test_pattern(runs, run_count, 0xA55AA55AA55AA55AULL,
                             true, true)) {
        memory_test_halt();
    }

    for (size_t i = 0; i < run_count; i++) {
        free_pages(runs[i].base, runs[i].pages);
    }
    if (pmm_free_pages() != page_count) {
        memory_test_report("FAIL: PMM page count did not recover after test");
        memory_test_halt();
    }
    free_mem(runs);
    memory_test_report("PASS: tested all free PMM pages");
    memory_test_halt();
}