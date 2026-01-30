#include <stdint.h>
#include "print.h"
#include "x86_64/allocator.h"

#define TEST_MEM_SIZE 4096
static uint8_t test_memory[TEST_MEM_SIZE];

void print_ptr(const char* prefix, void* ptr) {
    print_str(prefix);
    print_uint64_hex((uint64_t)ptr);
    print_str("\n");
}

void test() {
    print_str("Initializing allocator...\n");
    allocator_init(test_memory, TEST_MEM_SIZE);

    print_str("Allocating block A (100 bytes)...\n");
    void* blockA = alloc(100);
    print_ptr("Block A allocated at: ", blockA);

    print_str("Allocating block B (200 bytes)...\n");
    void* blockB = alloc(200);
    print_ptr("Block B allocated at: ", blockB);

    print_str("Freeing block A...\n");
    free_mem(blockA);

    print_str("Allocating block C (50 bytes)...\n");
    void* blockC = alloc(50);
    print_ptr("Block C allocated at: ", blockC);

    free_mem(blockB);
    free_mem(blockC);

    print_str("Allocator test complete.\n");
    return;
}
