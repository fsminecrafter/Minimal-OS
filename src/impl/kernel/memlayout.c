#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "x86_64/memlayout.h"
#include "mmu_tables.h"
#include "serial.h"
#include "string.h"
#include "x86_64/pmm.h"

/*
 * Where the kernel heap and the page pool may live.
 *
 * The old code assumed RAM was one block from the end of the kernel up to
 * "total RAM". On a real PC that is wrong twice over: RAM has holes (the
 * PCI hole below 4 GiB, ACPI tables, firmware areas), and with more than
 * about 1 GiB installed the top of RAM was not mapped at all. Here the
 * multiboot2 memory map is read properly: the heap goes into the biggest
 * free piece of available RAM below 4 GiB (the identity-mapped range)
 * that does not overlap the kernel or the boot modules GRUB loaded.
 */

#define MB2_TAG_MODULE 3u
#define MB2_TAG_MMAP   6u
#define MB2_MEM_AVAILABLE 1u
#define MB2_MEM_ACPI_RECLAIMABLE 3u
#define MB2_MEM_ACPI_NVS 4u

#define MB_COPY_SIZE (64u * 1024u)
static uint8_t g_mb_copy[MB_COPY_SIZE] __attribute__((aligned(8)));

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
} mb2_module_tag_t;

multiboot2_info_t* memlayout_save_multiboot(multiboot2_info_t* mb) {
    if (!mb || mb->total_size < 8 || mb->total_size > MB_COPY_SIZE) return mb;
    memcpy(g_mb_copy, mb, mb->total_size);
    return (multiboot2_info_t*)g_mb_copy;
}

typedef void (*tag_fn)(const multiboot2_tag_t* tag, void* ctx);

static void for_each_tag(multiboot2_info_t* mb, uint32_t type, tag_fn fn, void* ctx) {
    if (!mb) return;
    uintptr_t cursor = (uintptr_t)mb->tags;
    uintptr_t end = (uintptr_t)mb + mb->total_size;
    while (cursor + sizeof(multiboot2_tag_t) <= end) {
        const multiboot2_tag_t* tag = (const multiboot2_tag_t*)cursor;
        if (tag->type == 0 || tag->size < 8) break;
        if (tag->type == type) fn(tag, ctx);
        cursor += (tag->size + 7u) & ~7u;
    }
}

/* ---------------------------------------------------------------- modules */

typedef struct {
    const char* name;
    memlayout_module_t* out;
    bool found;
    uint32_t index;
    uint32_t want_index;
} module_find_t;

static bool module_name_matches(const char* cmdline, const char* name) {
    // The module's command line is "<name> [args]"; also match a path
    // ending in /<name>.
    size_t n = strlen(name);
    const char* word_end = cmdline;
    while (*word_end && *word_end != ' ') word_end++;
    size_t len = (size_t)(word_end - cmdline);
    if (len == n && strncmp(cmdline, name, n) == 0) return true;
    if (len > n && cmdline[len - n - 1] == '/' &&
        strncmp(cmdline + len - n, name, n) == 0) return true;
    return false;
}

static void module_find_cb(const multiboot2_tag_t* tag, void* ctx) {
    module_find_t* f = (module_find_t*)ctx;
    const mb2_module_tag_t* m = (const mb2_module_tag_t*)tag;
    uint32_t idx = f->index++;
    if (f->found) return;
    bool hit = f->name ? module_name_matches(m->cmdline, f->name)
                       : idx == f->want_index;
    if (!hit) return;
    f->found = true;
    f->out->start = m->mod_start;
    f->out->end = m->mod_end;
    strncpy(f->out->cmdline, m->cmdline, sizeof(f->out->cmdline) - 1);
    f->out->cmdline[sizeof(f->out->cmdline) - 1] = '\0';
}

bool memlayout_find_module(multiboot2_info_t* mb, const char* name,
                           memlayout_module_t* out) {
    if (!out || !name) return false;
    memset(out, 0, sizeof(*out));
    module_find_t f = { .name = name, .out = out };
    for_each_tag(mb, MB2_TAG_MODULE, module_find_cb, &f);
    return f.found;
}

bool memlayout_get_module(multiboot2_info_t* mb, uint32_t index,
                          memlayout_module_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    module_find_t f = { .name = NULL, .out = out, .want_index = index };
    for_each_tag(mb, MB2_TAG_MODULE, module_find_cb, &f);
    return f.found;
}

/* ---------------------------------------------------------------- regions */

#define MAX_RESERVED 24

typedef struct {
    uint64_t start, end;
} range_t;

typedef struct {
    range_t reserved[MAX_RESERVED];
    uint32_t reserved_count;
    uint64_t limit;
    uint64_t best_start, best_end;
} pick_t;

static void reserve_module_cb(const multiboot2_tag_t* tag, void* ctx) {
    pick_t* p = (pick_t*)ctx;
    const mb2_module_tag_t* m = (const mb2_module_tag_t*)tag;
    if (p->reserved_count < MAX_RESERVED) {
        p->reserved[p->reserved_count].start = m->mod_start & ~0xFFFull;
        p->reserved[p->reserved_count].end = ((uint64_t)m->mod_end + 0xFFF) & ~0xFFFull;
        p->reserved_count++;
    }
}

// Largest piece of [start, end) that avoids every reserved range.
static void consider(pick_t* p, uint64_t start, uint64_t end) {
    start = (start + 0xFFF) & ~0xFFFull;
    end &= ~0xFFFull;
    if (end <= start) return;
    for (uint32_t i = 0; i < p->reserved_count; i++) {
        range_t r = p->reserved[i];
        if (r.end <= start || r.start >= end) continue;
        // Split around the reserved range.
        consider(p, start, r.start);
        consider(p, r.end, end);
        return;
    }
    if (end - start > p->best_end - p->best_start) {
        p->best_start = start;
        p->best_end = end;
    }
}

static void mmap_pick_cb(const multiboot2_tag_t* tag, void* ctx) {
    pick_t* p = (pick_t*)ctx;
    const multiboot2_tag_mmap_t* mm = (const multiboot2_tag_mmap_t*)tag;
    if (mm->entry_size < sizeof(multiboot2_mmap_entry_t)) return;
    const uint8_t* e = (const uint8_t*)(mm + 1);
    const uint8_t* end = (const uint8_t*)tag + tag->size;
    for (; e + mm->entry_size <= end; e += mm->entry_size) {
        const multiboot2_mmap_entry_t* ent = (const multiboot2_mmap_entry_t*)e;
        if (ent->type != MB2_MEM_AVAILABLE) continue;
        uint64_t s = ent->base_addr, x = ent->base_addr + ent->length;
        if (x > p->limit) x = p->limit;
        if (s < x) consider(p, s, x);
    }
}

bool memlayout_pick_heap_region(multiboot2_info_t* mb, uintptr_t kernel_end,
                                uint64_t* start, uint64_t* end) {
    pick_t p;
    memset(&p, 0, sizeof(p));
    p.limit = (uint64_t)IDENTITY_MAP_GIB << 30;
    // Everything below the end of the kernel image stays untouched (BIOS
    // data, AP trampoline, the kernel itself).
    p.reserved[p.reserved_count++] = (range_t){ 0, ((uint64_t)kernel_end + 0xFFF) & ~0xFFFull };
    for_each_tag(mb, MB2_TAG_MODULE, reserve_module_cb, &p);
    for_each_tag(mb, MB2_TAG_MMAP, mmap_pick_cb, &p);
    if (p.best_end <= p.best_start) return false;
    *start = p.best_start;
    *end = p.best_end;
    return true;
}

/* ---------------------------------------------------------- cache policy */

typedef struct {
    uint8_t ram[IDENTITY_MAP_GIB * 512];    // one flag per 2 MiB page
} cache_t;

static void mmap_cache_cb(const multiboot2_tag_t* tag, void* ctx) {
    cache_t* c = (cache_t*)ctx;
    const multiboot2_tag_mmap_t* mm = (const multiboot2_tag_mmap_t*)tag;
    if (mm->entry_size < sizeof(multiboot2_mmap_entry_t)) return;
    const uint8_t* e = (const uint8_t*)(mm + 1);
    const uint8_t* end = (const uint8_t*)tag + tag->size;
    for (; e + mm->entry_size <= end; e += mm->entry_size) {
        const multiboot2_mmap_entry_t* ent = (const multiboot2_mmap_entry_t*)e;
        if (ent->type != MB2_MEM_AVAILABLE && ent->type != MB2_MEM_ACPI_RECLAIMABLE &&
            ent->type != MB2_MEM_ACPI_NVS) continue;
        if (ent->length == 0) continue;
        uint64_t first = ent->base_addr >> 21;
        uint64_t last = (ent->base_addr + ent->length - 1) >> 21;
        for (uint64_t i = first; i <= last && i < IDENTITY_MAP_GIB * 512u; i++) {
            c->ram[i] = 1;
        }
    }
}

void memlayout_apply_cache_policy(multiboot2_info_t* mb) {
    static cache_t c;
    memset(&c, 0, sizeof(c));
    for_each_tag(mb, MB2_TAG_MMAP, mmap_cache_cb, &c);
    c.ram[0] = 1;   // low memory always stays write-back
    uint32_t uncached = 0;
    for (uint32_t i = 0; i < IDENTITY_MAP_GIB * 512u; i++) {
        if (c.ram[i]) continue;
        // Device memory (PCI hole, APIC, firmware flash): never cache it.
        page_table_l2[i] |= (1ull << 3) | (1ull << 4);   // PWT | PCD
        uncached++;
    }
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    serial_write_str("memlayout: ");
    serial_write_dec(uncached);
    serial_write_str(" identity 2 MiB pages marked uncached (no RAM)\n");
}

/* ------------------------------------------------------- RAM above 4 GiB */

// The boot page tables identity map the first 4 GiB only. RAM beyond that
// is mapped here the same way (2 MiB pages, in the kernel's own tables,
// which every process shares) and handed to the page pool for program
// memory. One PML4 slot covers 512 GiB; program memory starts right after
// it (uvm.h), so that is the limit.
#define HIGH_RAM_LIMIT (512ull << 30)
#define HIGH_RAM_MIN   (4ull << 20)            // not worth a bitmap below this

typedef struct {
    uint64_t pages;
    uint32_t regions;
} high_ram_t;

static bool map_high_2m(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += 0x200000ull) {
        uint64_t gib = addr >> 30;
        if (!(page_table_l3[gib] & 1)) {
            void* pd = pmm_try_alloc_pages_zeroed(1);
            if (!pd) return false;
            page_table_l3[gib] = (uint64_t)(uintptr_t)pd | 0x3;
        }
        uint64_t* pd = (uint64_t*)(uintptr_t)(page_table_l3[gib] & 0x000FFFFFFFFFF000ull);
        // Present, writable, 2 MiB, never executable.
        pd[(addr >> 21) & 0x1FF] = addr | 0x83 | (1ull << 63);
    }
    return true;
}

static void high_ram_cb(const multiboot2_tag_t* tag, void* ctx) {
    high_ram_t* h = (high_ram_t*)ctx;
    const multiboot2_tag_mmap_t* mm = (const multiboot2_tag_mmap_t*)tag;
    if (mm->entry_size < sizeof(multiboot2_mmap_entry_t)) return;
    const uint8_t* e = (const uint8_t*)(mm + 1);
    const uint8_t* end = (const uint8_t*)tag + tag->size;
    for (; e + mm->entry_size <= end; e += mm->entry_size) {
        const multiboot2_mmap_entry_t* ent = (const multiboot2_mmap_entry_t*)e;
        if (ent->type != MB2_MEM_AVAILABLE) continue;
        uint64_t s = ent->base_addr, x = ent->base_addr + ent->length;
        if (x < s) continue;
        if (s < ((uint64_t)IDENTITY_MAP_GIB << 30)) s = (uint64_t)IDENTITY_MAP_GIB << 30;
        if (x > HIGH_RAM_LIMIT) x = HIGH_RAM_LIMIT;
        // Whole 2 MiB pages only, so no mapping reaches past the RAM.
        s = (s + 0x1FFFFFull) & ~0x1FFFFFull;
        x &= ~0x1FFFFFull;
        if (x <= s || x - s < HIGH_RAM_MIN) continue;
        if (!map_high_2m(s, x)) {
            serial_write_str("memlayout: no memory to map RAM above 4 GiB\n");
            continue;
        }
        if (pmm_add_high_region((uintptr_t)s, (size_t)(x - s))) {
            h->pages += (x - s) >> 12;
            h->regions++;
        }
    }
}

void memlayout_add_high_ram(multiboot2_info_t* mb) {
    high_ram_t h = { 0, 0 };
    for_each_tag(mb, MB2_TAG_MMAP, high_ram_cb, &h);
    // The new entries only add mappings; nothing cached needs flushing.
    serial_write_str("memlayout: ");
    serial_write_dec(h.pages >> 8);
    serial_write_str(" MiB of RAM above 4 GiB in ");
    serial_write_dec(h.regions);
    serial_write_str(" region(s) for program memory\n");
}
