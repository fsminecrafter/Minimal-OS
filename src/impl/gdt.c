#include "x86_64/gdt.h"
#include "x86_64/tss.h"
#include "x86_64/smp.h"
#include "x86_64/allocator.h"
#include "string.h"
#include "serial.h"
#include "panic.h"

#define ACC_PRESENT   (1 << 7)
#define ACC_CODEDATA  (1 << 4)
#define ACC_EXEC      (1 << 3)
#define ACC_RW        (1 << 1)
#define TSS_TYPE_AVAIL 0x9

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} gdt_tss_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

#define GDT_FIXED_ENTRIES 3   // null, code, data (8 bytes each)
#define GDT_TOTAL_QWORDS  (GDT_FIXED_ENTRIES + MAX_CPUS * 2)

static uint64_t   g_gdt[GDT_TOTAL_QWORDS] __attribute__((aligned(16)));
static gdt_ptr_t   g_gdt_ptr;
static tss_entry_t g_tss[MAX_CPUS] __attribute__((aligned(16)));

static void gdt_set_entry(int index, uint8_t access, uint8_t flags) {
    gdt_entry_t e = {0};
    e.limit_low   = 0xFFFF;
    e.access      = access;
    e.granularity = 0x0F | (flags << 4);
    memcpy(&g_gdt[index], &e, sizeof(e));
}

static void gdt_set_tss(int cpu, tss_entry_t* tss) {
    uintptr_t base  = (uintptr_t)tss;
    uint32_t  limit = sizeof(tss_entry_t) - 1;

    gdt_tss_entry_t e = {0};
    e.limit_low   = limit & 0xFFFF;
    e.base_low    = base & 0xFFFF;
    e.base_mid    = (base >> 16) & 0xFF;
    e.access      = ACC_PRESENT | TSS_TYPE_AVAIL;   // S=0 (system segment)
    e.granularity = (limit >> 16) & 0x0F;
    e.base_high   = (base >> 24) & 0xFF;
    e.base_upper  = (uint32_t)(base >> 32);

    memcpy(&g_gdt[GDT_FIXED_ENTRIES + cpu * 2], &e, sizeof(e));
}

static void gdt_flush(gdt_ptr_t* ptr) {
    asm volatile(
        "lgdt (%0)\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :: "r"(ptr) : "rax", "memory"
    );
}

void ltr(uint16_t sel) {
    asm volatile("ltr %0" :: "r"(sel));
}

void gdt_init(void) {
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(g_tss, 0, sizeof(g_tss));

    gdt_set_entry(1, ACC_PRESENT | ACC_CODEDATA | ACC_EXEC | ACC_RW, 0x2); // 0x08: 64-bit code (L=1)
    gdt_set_entry(2, ACC_PRESENT | ACC_CODEDATA | ACC_RW,            0xC); // 0x10: data

    for (int i = 0; i < MAX_CPUS; i++) {
        // Per-core double-fault (IST1) stack. Previously every core
        // would have shared main.asm's single static
        // double_fault_stack - two cores double-faulting at once
        // would have stomped each other's register dump.
        void* df_stack = alloc(4096);
        if (!df_stack) PANIC("gdt_init: OOM allocating per-CPU IST1 stack");
        g_tss[i].ist[0] = (uint64_t)df_stack + 4096;
        gdt_set_tss(i, &g_tss[i]);
    }

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base  = (uint64_t)&g_gdt;

    gdt_flush(&g_gdt_ptr);
    ltr(GDT_TSS_SELECTOR(0));

    serial_write_str("GDT: initialized, ");
    serial_write_dec(MAX_CPUS);
    serial_write_str(" TSS slots ready\n");
}

void gdt_load_this_cpu(uint32_t cpu_id) {
    gdt_flush(&g_gdt_ptr);
    ltr(GDT_TSS_SELECTOR(cpu_id));
}

tss_entry_t* gdt_get_tss(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return NULL;
    return &g_tss[cpu_id];
}