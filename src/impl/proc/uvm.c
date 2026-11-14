// src/impl/proc/uvm.c - user virtual memory (see uvm.h)
#include "x86_64/uvm.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/pmm.h"
#include "x86_64/allocator.h"
#include "x86_64/syscall.h"
#include "x86_64/tlb.h"
#include "x86_64/smp.h"
#include "string.h"
#include "serial.h"

#define PAGE UVM_PAGE_SIZE

// Page-table entry bits.
#define PTE_P     (1ULL << 0)
#define PTE_W     (1ULL << 1)
#define PTE_U     (1ULL << 2)
#define PTE_HUGE  (1ULL << 7)
// Software bit: the entry holds a frame but is not present, because its
// area has no access (UVM_PROT_NONE). The contents wait for the next
// uvm_protect().
#define PTE_HELD  (1ULL << 9)
// Software bit: the frame belongs to a shared object (uvm_shm_t), not to
// this space. Such an entry is never freed with its page; the area's
// reference on the object is what is given up when the mapping goes.
#define PTE_SHARED (1ULL << 10)
#define PTE_NX    (1ULL << 63)
#define PTE_ADDR  0x000FFFFFFFFFF000ULL

// Long operations take the lock for this much address space at a time.
#define UVM_CHUNK (2ull << 20)

#define SLOT_FIRST ((UVM_MIN >> 39) & 0x1FF)
#define SLOT_LAST  (((UVM_MAX - 1) >> 39) & 0x1FF)

typedef struct uvm_area {
    uint64_t start, end;            // page aligned, end exclusive
    uint32_t prot;
    uvm_shm_t* shm;                 // shared object behind the area, or NULL
    uint32_t max_prot;              // shared areas: what uvm_protect() may grant
    struct uvm_area* next;          // sorted by address, never overlapping
} uvm_area_t;

// A reference-counted run of frames from the page pool that can be mapped
// into several spaces. The frames are contiguous (so the kernel has one
// linear view of them through the identity map) and zeroed when created.
// One reference belongs to whoever created it (the kernel), one to every
// area that maps it; the frames go back to the pool with the last one.
struct uvm_shm {
    volatile int refs;
    uint64_t pages;
    uint8_t* base;
};

typedef struct uvm_space {
    uvm_area_t* areas;
    uint64_t* pml4;
    volatile int lock;
    // CPUs that may have this space's page tables loaded (bit per CPU);
    // see uvm_cpu_switch() and tlb.h.
    volatile uint32_t loaded_cpus;
    uint64_t committed_pages;
    uint64_t mapped_pages;
} uvm_space_t;

static inline uint64_t space_lock(uvm_space_t* s) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&s->lock, 1)) { tlb_poll(); __asm__ volatile("pause"); }
    return flags;
}

static inline void space_unlock(uvm_space_t* s, uint64_t flags) {
    __sync_lock_release(&s->lock);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// System calls run with interrupts off. Loops over many pages open a
// window for them now and then, so that a call working through gigabytes
// does not starve the timer (and trip the hang watchdog).
static inline void let_interrupts_in(void) {
    __asm__ volatile("pushfq; sti; nop; nop; popfq" ::: "memory", "cc");
}

static uvm_space_t* space_of(process_t* proc) {
    proc = proc_mem_owner(proc);
    return proc ? proc->uvm : NULL;
}

static inline bool space_is_loaded(const uvm_space_t* s) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (cr3 & PTE_ADDR) == (uint64_t)(uintptr_t)s->pml4;
}

// Pages whose entries were removed or reduced under the space lock, and
// the frames to give back once every CPU has flushed them (never before:
// another CPU may still write through a stale entry).
#define FLUSH_BATCH 128
typedef struct {
    uvm_space_t* s;
    uint64_t lo, hi;                // address range touched
    uint32_t n;
    void* frames[FLUSH_BATCH];
} flushctx_t;

static void fc_init(flushctx_t* fc, uvm_space_t* s) {
    fc->s = s; fc->lo = ~0ULL; fc->hi = 0; fc->n = 0;
}

static void fc_note(flushctx_t* fc, uint64_t addr) {
    if (addr < fc->lo) fc->lo = addr;
    if (addr + PAGE > fc->hi) fc->hi = addr + PAGE;
}

// Flushes the range on every CPU that has the space loaded, then frees the
// collected frames. Called with the space lock held.
static void fc_flush(flushctx_t* fc) {
    if (fc->hi > fc->lo)
        tlb_shootdown(&fc->s->loaded_cpus, (uint64_t)(uintptr_t)fc->s->pml4, fc->lo, fc->hi);
    for (uint32_t i = 0; i < fc->n; i++) pmm_free_page_unscrubbed(fc->frames[i]);
    fc->n = 0; fc->lo = ~0ULL; fc->hi = 0;
}

// Flushes [start, end) of the space on every CPU that may have it loaded
// (also this one), for page-table changes made outside this file (SYS_HEAP).
void uvm_flush_range(process_t* proc, uint64_t start, uint64_t end) {
    uvm_space_t* s = space_of(proc);
    if (s) tlb_shootdown(&s->loaded_cpus, (uint64_t)(uintptr_t)s->pml4, start, end);
}

// Called by the scheduler, interrupts off, around every address space
// change on this CPU: sets the bit of the space about to be loaded before
// its CR3 is, clears the bit of the one being left (no user memory is
// touched in between, and the CR3 load flushes it).
void uvm_cpu_switch(process_t* from, process_t* to) {
    uvm_space_t* old = (from && from->privilege == PROC_PRIVILEGE_USER) ? space_of(from) : NULL;
    uvm_space_t* now = (to && to->privilege == PROC_PRIVILEGE_USER) ? space_of(to) : NULL;
    if (old == now) return;
    uint32_t bit = 1u << smp_current_cpu_id();
    if (now) __atomic_fetch_or(&now->loaded_cpus, bit, __ATOMIC_SEQ_CST);
    if (old) __atomic_fetch_and(&old->loaded_cpus, ~bit, __ATOMIC_SEQ_CST);
}

static inline uint64_t leaf_flags(uint32_t prot) {
    if (!(prot & (UVM_PROT_READ | UVM_PROT_WRITE | UVM_PROT_EXEC))) return PTE_HELD;
    uint64_t f = PTE_P | PTE_U;
    if (prot & UVM_PROT_WRITE) f |= PTE_W;
    if (!(prot & UVM_PROT_EXEC)) f |= PTE_NX;
    return f;
}

static inline bool pte_has_frame(uint64_t pte) {
    return (pte & (PTE_P | PTE_HELD)) != 0;
}

// The entry for `addr`, or NULL when a table on the way is missing and
// `create` is false (or a table could not be allocated).
static uint64_t* walk(uvm_space_t* s, uint64_t addr, bool create) {
    uint64_t* table = s->pml4;
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t* entry = &table[(addr >> shift) & 0x1FF];
        if (!(*entry & PTE_P)) {
            if (!create) return NULL;
            void* next = pmm_try_alloc_page();
            if (!next) return NULL;
            memset(next, 0, PAGE);
            // Access is decided at the leaf.
            *entry = (uint64_t)(uintptr_t)next | PTE_P | PTE_W | PTE_U;
        }
        table = (uint64_t*)(uintptr_t)(*entry & PTE_ADDR);
    }
    return &table[(addr >> 12) & 0x1FF];
}

// The area containing `addr`, or NULL.
static uvm_area_t* area_at(uvm_space_t* s, uint64_t addr) {
    for (uvm_area_t* a = s->areas; a && a->start <= addr; a = a->next)
        if (addr < a->end) return a;
    return NULL;
}

// True when [start, end) is mapped without a hole and every area in it
// allows `need`.
static bool covered(uvm_space_t* s, uint64_t start, uint64_t end, uint32_t need) {
    uint64_t at = start;
    for (uvm_area_t* a = s->areas; a && at < end; a = a->next) {
        if (a->end <= at) continue;
        if (a->start > at) return false;
        if ((a->prot & need) != need) return false;
        at = a->end;
    }
    return at >= end;
}

// The frame behind `addr`, allocated (zeroed) now when the page has none.
// `prot` is the area's. NULL when memory ran out.
static uint8_t* frame_for(uvm_space_t* s, uint64_t addr, uint32_t prot) {
    uint64_t* pte = walk(s, addr, true);
    if (!pte) return NULL;
    if (pte_has_frame(*pte)) return (uint8_t*)(uintptr_t)(*pte & PTE_ADDR);
    void* frame = pmm_try_alloc_page();
    if (!frame) return NULL;
    memset(frame, 0, PAGE);
    *pte = (uint64_t)(uintptr_t)frame | leaf_flags(prot);
    s->committed_pages++;
    return (uint8_t*)frame;
}

// Calls fn for every entry of [start, end) that holds a frame, skipping
// missing tables a whole table at a time.
typedef void (*pte_fn)(uvm_space_t* s, uint64_t addr, uint64_t* pte, flushctx_t* fc, uint32_t arg);

static void for_each_frame(uvm_space_t* s, uint64_t start, uint64_t end, pte_fn fn, flushctx_t* fc, uint32_t arg) {
    uint64_t addr = start;
    while (addr < end) {
        uint64_t* table = s->pml4;
        int shift;
        for (shift = 39; shift > 12; shift -= 9) {
            uint64_t entry = table[(addr >> shift) & 0x1FF];
            if (!(entry & PTE_P)) break;
            table = (uint64_t*)(uintptr_t)(entry & PTE_ADDR);
        }
        if (shift > 12) {
            // Nothing below this entry: on to the next one at this level.
            uint64_t span = 1ULL << shift;
            uint64_t next = (addr & ~(span - 1)) + span;
            if (next <= addr) break;
            addr = next;
            continue;
        }
        uint64_t* pte = &table[(addr >> 12) & 0x1FF];
        if (pte_has_frame(*pte)) fn(s, addr, pte, fc, arg);
        addr += PAGE;
    }
}

static void release_frame(uvm_space_t* s, uint64_t addr, uint64_t* pte, flushctx_t* fc, uint32_t arg) {
    (void)arg;
    // Shared frames are not ours to free (callers keep shared areas out of
    // their ranges; this is the last line of defence).
    if (*pte & PTE_SHARED) return;
    void* frame = (void*)(uintptr_t)(*pte & PTE_ADDR);
    *pte = 0;
    s->committed_pages--;
    fc_note(fc, addr);
    fc->frames[fc->n++] = frame;
    if (fc->n == FLUSH_BATCH) fc_flush(fc);
}

// Takes a shared frame out of the page tables without freeing it.
static void release_frame_shared(uvm_space_t* s, uint64_t addr, uint64_t* pte, flushctx_t* fc, uint32_t arg) {
    (void)s; (void)arg;
    *pte = 0;
    fc_note(fc, addr);
}

// Frees the page tables that lie wholly inside [start, end), once their
// frames are gone, so that mapping and unmapping does not pile up tables.
// Tables that the range only partly covers stay (other mappings may use
// them); uvm_destroy frees those.
// Page-table pages that were unlinked, waiting for the flush: another CPU's
// paging-structure cache may still point at them.
#define PRUNE_BATCH 64
typedef struct {
    uvm_space_t* s;
    uint32_t n;
    void* pages[PRUNE_BATCH];
} prunectx_t;

static void prune_flush(prunectx_t* pc) {
    if (!pc->n) return;
    tlb_shootdown(&pc->s->loaded_cpus, (uint64_t)(uintptr_t)pc->s->pml4, 0, 0);
    for (uint32_t i = 0; i < pc->n; i++) pmm_free_page_unscrubbed(pc->pages[i]);
    pc->n = 0;
}

static void prune_tables(prunectx_t* pc, uint64_t* table, int shift, uint64_t table_base,
                         uint64_t start, uint64_t end) {
    uint64_t span = 1ULL << shift;              // what one entry of `table` covers
    for (int i = 0; i < 512; i++) {
        uint64_t lo = table_base + (uint64_t)i * span;
        uint64_t hi = lo + span;
        if (hi <= start) continue;
        if (lo >= end) break;
        uint64_t entry = table[i];
        if (!(entry & PTE_P)) continue;
        uint64_t* child = (uint64_t*)(uintptr_t)(entry & PTE_ADDR);
        if (lo >= start && hi <= end) {
            // Everything below is unmapped: page tables only, no frames.
            if (shift > 21) prune_tables(pc, child, shift - 9, lo, lo, hi);
            table[i] = 0;
            pc->pages[pc->n++] = child;
            if (pc->n == PRUNE_BATCH) prune_flush(pc);
        } else if (shift > 21) {
            prune_tables(pc, child, shift - 9, lo, start, end);
        }
    }
}

static void reprotect_frame(uvm_space_t* s, uint64_t addr, uint64_t* pte, flushctx_t* fc, uint32_t prot) {
    (void)s;
    uint64_t old = *pte;
    *pte = (old & (PTE_ADDR | PTE_SHARED)) | leaf_flags(prot);
    // Entries that do not change need no flush.
    if (*pte != (old & (PTE_ADDR | PTE_P | PTE_W | PTE_U | PTE_NX | PTE_HELD | PTE_SHARED))) fc_note(fc, addr);
}

static inline bool range_valid(uint64_t addr, uint64_t len) {
    return !(addr & (PAGE - 1)) && len && addr >= UVM_MIN && addr < UVM_MAX &&
           len <= UVM_MAX - addr;
}

static inline uint64_t round_up(uint64_t v) {
    return (v + PAGE - 1) & ~(PAGE - 1);
}

// --------------------------------------------------------------- lifecycle

bool uvm_create(process_t* proc) {
    if (!proc || !proc->pml4 || proc->uvm) return false;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)proc->pml4;
    // The kernel keeps these slots empty; they are the program's.
    for (uint64_t i = SLOT_FIRST; i <= SLOT_LAST; i++)
        if (pml4[i]) return false;
    uvm_space_t* s = (uvm_space_t*)alloc(sizeof(*s));
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    s->pml4 = pml4;
    proc->uvm = s;
    return true;
}

static void free_tables(uint64_t* table, int level) {
    for (int i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (level == 1) {
            if (pte_has_frame(entry) && !(entry & PTE_SHARED)) pmm_free_page_unscrubbed((void*)(uintptr_t)(entry & PTE_ADDR));
            continue;
        }
        if (!(entry & PTE_P) || (entry & PTE_HUGE)) continue;
        free_tables((uint64_t*)(uintptr_t)(entry & PTE_ADDR), level - 1);
    }
    pmm_free_page_unscrubbed(table);
}

void uvm_destroy(process_t* proc) {
    if (!proc || proc->mem_owner || !proc->uvm) return;
    uvm_space_t* s = proc->uvm;
    proc->uvm = NULL;
    for (uint64_t i = SLOT_FIRST; i <= SLOT_LAST; i++) {
        uint64_t entry = s->pml4[i];
        s->pml4[i] = 0;
        if (entry & PTE_P) free_tables((uint64_t*)(uintptr_t)(entry & PTE_ADDR), 3);
    }
    for (uvm_area_t* a = s->areas; a;) {
        uvm_area_t* next = a->next;
        if (a->shm) uvm_shm_put(a->shm);
        free_mem(a);
        a = next;
    }
    free_mem(s);
}

// ------------------------------------------------------------------ mapping

// Where a mapping of `len` bytes fits at or above `from`, with a free page
// on both sides. 0 when the space is full.
static uint64_t find_room(uvm_space_t* s, uint64_t from, uint64_t len) {
    uint64_t at = from;
    for (uvm_area_t* a = s->areas; a; a = a->next) {
        if (a->end + PAGE <= at) continue;
        if (at + len + PAGE <= a->start) break;
        at = a->end + PAGE;
        if (at > UVM_MAX) return 0;
    }
    return (len <= UVM_MAX - at) ? at : 0;
}

static void insert_area(uvm_space_t* s, uvm_area_t* n) {
    uvm_area_t** link = &s->areas;
    while (*link && (*link)->start < n->start) link = &(*link)->next;
    n->next = *link;
    *link = n;
    s->mapped_pages += (n->end - n->start) / PAGE;
}

static bool overlaps(uvm_space_t* s, uint64_t start, uint64_t end) {
    for (uvm_area_t* a = s->areas; a && a->start < end; a = a->next)
        if (a->end > start) return true;
    return false;
}

// Removes [start, end) from the area list. `spare` is used when an area
// has to be split in two; returns it, or NULL once used.
static uvm_area_t* carve(uvm_space_t* s, uint64_t start, uint64_t end, uvm_area_t* spare) {
    uvm_area_t** link = &s->areas;
    while (*link && (*link)->start < end) {
        uvm_area_t* a = *link;
        if (a->end <= start) { link = &a->next; continue; }
        uint64_t cut_start = a->start > start ? a->start : start;
        uint64_t cut_end = a->end < end ? a->end : end;
        s->mapped_pages -= (cut_end - cut_start) / PAGE;
        if (cut_start == a->start && cut_end == a->end) {
            *link = a->next;
            free_mem(a);
            continue;
        }
        if (cut_start == a->start) { a->start = cut_end; break; }
        if (cut_end == a->end) { a->end = cut_start; link = &a->next; continue; }
        // A hole in the middle: the tail becomes an area of its own.
        spare->start = cut_end;
        spare->end = a->end;
        spare->prot = a->prot;
        spare->shm = NULL;          // shared areas are never cut in the middle
        spare->max_prot = 0;
        spare->next = a->next;
        a->end = cut_start;
        a->next = spare;
        spare = NULL;
        break;
    }
    return spare;
}

// Makes `at` an area boundary when it falls inside one. Uses *spare.
static void split_at(uvm_space_t* s, uint64_t at, uvm_area_t** spare) {
    uvm_area_t* a = area_at(s, at);
    if (!a || a->start == at) return;
    uvm_area_t* n = *spare;
    *spare = NULL;
    n->start = at;
    n->end = a->end;
    n->prot = a->prot;
    n->shm = NULL;                  // shared areas are never split
    n->max_prot = 0;
    n->next = a->next;
    a->end = at;
    a->next = n;
}

uint64_t uvm_map(process_t* proc, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags) {
    uvm_space_t* s = space_of(proc);
    if (!s || !len || len > UVM_MAX - UVM_MIN) return SYS_ERR_INVAL;
    if (prot & ~(UVM_PROT_READ | UVM_PROT_WRITE | UVM_PROT_EXEC)) return SYS_ERR_INVAL;
    if (flags & ~(UVM_MAP_FIXED | UVM_MAP_POPULATE)) return SYS_ERR_INVAL;
    len = round_up(len);
    if ((flags & UVM_MAP_FIXED) && !range_valid(addr, len)) return SYS_ERR_INVAL;

    uvm_area_t* n = (uvm_area_t*)alloc(sizeof(*n));
    if (!n) return SYS_ERR_NOMEM;

    uint64_t irq = space_lock(s);
    uint64_t start;
    if (flags & UVM_MAP_FIXED) {
        start = overlaps(s, addr, addr + len) ? 0 : addr;
    } else {
        uint64_t from = UVM_MMAP_BASE;
        if (addr >= UVM_MIN && addr < UVM_MAX) from = round_up(addr);
        start = find_room(s, from, len);
        // A hint too high to fit below it: anywhere will do.
        if (!start && from != UVM_MMAP_BASE) start = find_room(s, UVM_MMAP_BASE, len);
    }
    if (!start) {
        space_unlock(s, irq);
        free_mem(n);
        return (flags & UVM_MAP_FIXED) ? SYS_ERR_INVAL : SYS_ERR_NOMEM;
    }
    n->start = start;
    n->end = start + len;
    n->prot = prot;
    n->shm = NULL;
    n->max_prot = 0;
    insert_area(s, n);
    space_unlock(s, irq);

    if (flags & UVM_MAP_POPULATE) {
        for (uint64_t page = start; page < start + len; page += PAGE) {
            irq = space_lock(s);
            bool ok = frame_for(s, page, prot) != NULL;
            space_unlock(s, irq);
            if (page > start && !(page & (UVM_CHUNK - 1))) let_interrupts_in();
            if (!ok) {
                uvm_unmap(proc, start, len);
                return SYS_ERR_NOMEM;
            }
        }
    }
    return start;
}

uint64_t uvm_unmap(process_t* proc, uint64_t addr, uint64_t len) {
    uvm_space_t* s = space_of(proc);
    if (!s || !range_valid(addr, len)) return SYS_ERR_INVAL;
    uint64_t end = addr + round_up(len);
    bool failed = false;            // a shared area was only partly in range
    uvm_area_t* spare = (uvm_area_t*)alloc(sizeof(*spare));
    if (!spare) return SYS_ERR_NOMEM;

    // A piece at a time, so that unmapping gigabytes never keeps
    // interrupts off for long. Each piece leaves the area list and loses
    // its frames in one step.
    for (uint64_t at = addr; at < end;) {
        uint64_t irq = space_lock(s);
        // Straight to the next area: there is nothing in between.
        uvm_area_t* next = s->areas;
        while (next && next->end <= at) next = next->next;
        if (!next || next->start >= end) {
            space_unlock(s, irq);
            break;
        }
        if (next->start > at) at = next->start;
        if (next->shm) {
            // A shared area goes whole or not at all: its reference is
            // given up with the mapping, and its frames stay with the
            // object (other spaces may map them).
            uvm_shm_t* shm = next->shm;
            uint64_t a_start = next->start, a_end = next->end;
            bool whole = a_start >= addr && a_end <= end;
            if (whole) {
                carve(s, a_start, a_end, NULL);
                flushctx_t fc;
                fc_init(&fc, s);
                for_each_frame(s, a_start, a_end, release_frame_shared, &fc, 0);
                fc_flush(&fc);
            } else {
                failed = true;
            }
            space_unlock(s, irq);
            if (whole) uvm_shm_put(shm);
            let_interrupts_in();
            at = a_end;
            continue;
        }
        uint64_t piece_end = (at & ~(UVM_CHUNK - 1)) + UVM_CHUNK;
        if (piece_end > end) piece_end = end;
        // Stop short of a shared area that begins inside the piece.
        for (uvm_area_t* a = next->next; a && a->start < piece_end; a = a->next)
            if (a->shm) { piece_end = a->start; break; }
        spare = carve(s, at, piece_end, spare);
        flushctx_t fc;
        fc_init(&fc, s);
        for_each_frame(s, at, piece_end, release_frame, &fc, 0);
        fc_flush(&fc);
        space_unlock(s, irq);
        let_interrupts_in();
        at = piece_end;
    }

    uint64_t irq = space_lock(s);
    prunectx_t pc;
    pc.s = s;
    pc.n = 0;
    // A shared area that was only partly in range is still mapped, perhaps
    // through a table that lies inside the range: prune nothing then.
    for (uint64_t slot = (addr >> 39) & 0x1FF; !failed && slot <= ((end - 1) >> 39) & 0x1FF; slot++) {
        uint64_t entry = s->pml4[slot];
        if (entry & PTE_P)
            prune_tables(&pc, (uint64_t*)(uintptr_t)(entry & PTE_ADDR), 30, slot << 39, addr, end);
    }
    // CPUs may have cached the freed tables themselves: flush everywhere
    // before the pages can be reused.
    prune_flush(&pc);
    space_unlock(s, irq);
    if (spare) free_mem(spare);
    return failed ? SYS_ERR_INVAL : SYS_SUCCESS;
}

uint64_t uvm_protect(process_t* proc, uint64_t addr, uint64_t len, uint32_t prot) {
    uvm_space_t* s = space_of(proc);
    if (!s || !range_valid(addr, len)) return SYS_ERR_INVAL;
    if (prot & ~(UVM_PROT_READ | UVM_PROT_WRITE | UVM_PROT_EXEC)) return SYS_ERR_INVAL;
    uint64_t end = addr + round_up(len);
    uvm_area_t* spare1 = (uvm_area_t*)alloc(sizeof(*spare1));
    uvm_area_t* spare2 = (uvm_area_t*)alloc(sizeof(*spare2));
    if (!spare1 || !spare2) {
        if (spare1) free_mem(spare1);
        if (spare2) free_mem(spare2);
        return SYS_ERR_NOMEM;
    }

    uint64_t irq = space_lock(s);
    uint64_t result = SYS_ERR_INVAL;
    bool shared_ok = true;
    // A shared area changes protection whole (it is never split), and never
    // beyond what it was mapped with; it can not become executable.
    for (uvm_area_t* a = s->areas; a && a->start < end; a = a->next) {
        if (a->end <= addr || !a->shm) continue;
        if (a->start < addr || a->end > end || (prot & ~a->max_prot)) shared_ok = false;
    }
    if (shared_ok && covered(s, addr, end, 0)) {
        split_at(s, addr, &spare1);
        split_at(s, end, &spare2);
        for (uvm_area_t* a = area_at(s, addr); a && a->start < end; a = a->next) a->prot = prot;
        result = SYS_SUCCESS;
    }
    space_unlock(s, irq);
    // The pages that already have a frame follow, a piece at a time.
    for (uint64_t at = addr; result == SYS_SUCCESS && at < end;) {
        uint64_t piece_end = (at & ~(UVM_CHUNK - 1)) + UVM_CHUNK;
        if (piece_end > end) piece_end = end;
        irq = space_lock(s);
        flushctx_t fc;
        fc_init(&fc, s);
        // Only where the area still says so: it may have changed meanwhile.
        for (uvm_area_t* a = s->areas; a && a->start < piece_end; a = a->next) {
            if (a->end <= at) continue;
            uint64_t lo = a->start > at ? a->start : at;
            uint64_t hi = a->end < piece_end ? a->end : piece_end;
            for_each_frame(s, lo, hi, reprotect_frame, &fc, a->prot);
        }
        fc_flush(&fc);
        space_unlock(s, irq);
        let_interrupts_in();
        at = piece_end;
    }
    if (spare1) free_mem(spare1);
    if (spare2) free_mem(spare2);
    return result;
}

uint64_t uvm_decommit(process_t* proc, uint64_t addr, uint64_t len) {
    uvm_space_t* s = space_of(proc);
    if (!s || !range_valid(addr, len)) return SYS_ERR_INVAL;
    uint64_t end = addr + round_up(len);
    uint64_t irq = space_lock(s);
    bool ok = covered(s, addr, end, 0);
    // Frames of a shared area belong to the object, not to this space:
    // decommit is refused there (SYS_ERR_INVAL).
    for (uvm_area_t* a = s->areas; ok && a && a->start < end; a = a->next)
        if (a->end > addr && a->shm) ok = false;
    space_unlock(s, irq);
    if (!ok) return SYS_ERR_INVAL;
    for (uint64_t at = addr; at < end;) {
        uint64_t piece_end = (at & ~(UVM_CHUNK - 1)) + UVM_CHUNK;
        if (piece_end > end) piece_end = end;
        irq = space_lock(s);
        flushctx_t fc;
        fc_init(&fc, s);
        for_each_frame(s, at, piece_end, release_frame, &fc, 0);
        fc_flush(&fc);
        space_unlock(s, irq);
        let_interrupts_in();
        at = piece_end;
    }
    return SYS_SUCCESS;
}

void uvm_release_all(process_t* proc) {
    if (!proc || proc->mem_owner || !proc->uvm) return;
    uvm_unmap(proc, UVM_MIN, UVM_MAX - UVM_MIN);
}

// ------------------------------------------------------------------- access

bool uvm_range_ok(process_t* proc, const void* ptr, size_t len, bool write) {
    uvm_space_t* s = space_of(proc);
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    if (!s || !len || addr < UVM_MIN || addr >= UVM_MAX || len > UVM_MAX - addr) return false;
    uint64_t start = addr & ~(PAGE - 1);
    uint64_t end = round_up(addr + len);
    uint32_t need = UVM_PROT_READ | (write ? UVM_PROT_WRITE : 0u);

    for (uint64_t page = start; page < end; page += PAGE) {
        uint64_t irq = space_lock(s);
        uvm_area_t* a = area_at(s, page);
        bool ok = a && (a->prot & need) == need && frame_for(s, page, a->prot);
        space_unlock(s, irq);
        // Only for ranges of 2 MiB and more: a caller with a small buffer may
        // hold a lock of its own.
        if (page - start >= UVM_CHUNK && !(page & (UVM_CHUNK - 1))) let_interrupts_in();
        if (!ok) return false;
    }
    return true;
}

bool uvm_is_executable(process_t* proc, uint64_t addr) {
    uvm_space_t* s = space_of(proc);
    if (!s) return false;
    uint64_t irq = space_lock(s);
    uvm_area_t* a = area_at(s, addr);
    bool ok = a && (a->prot & UVM_PROT_EXEC);
    space_unlock(s, irq);
    return ok;
}

static bool copy(process_t* proc, uint64_t addr, void* buffer, size_t len, bool in) {
    uvm_space_t* s = space_of(proc);
    if (!s || addr < UVM_MIN || addr >= UVM_MAX || len > UVM_MAX - addr) return false;
    uint8_t* bytes = (uint8_t*)buffer;
    while (len) {
        uint64_t page = addr & ~(PAGE - 1);
        size_t offset = (size_t)(addr - page);
        size_t chunk = PAGE - offset;
        if (chunk > len) chunk = len;

        uint64_t irq = space_lock(s);
        uvm_area_t* a = area_at(s, page);
        uint8_t* frame = a ? frame_for(s, page, a->prot) : NULL;
        space_unlock(s, irq);
        if (!frame) return false;
        // Frames are identity mapped in every address space.
        if (in) memcpy(frame + offset, bytes, chunk);
        else    memcpy(bytes, frame + offset, chunk);

        addr += chunk;
        bytes += chunk;
        len -= chunk;
    }
    return true;
}

bool uvm_copy_in(process_t* proc, uint64_t addr, const void* src, size_t len) {
    return copy(proc, addr, (void*)(uintptr_t)src, len, true);
}

bool uvm_copy_out(process_t* proc, uint64_t addr, void* dst, size_t len) {
    return copy(proc, addr, dst, len, false);
}

// -------------------------------------------------------------- page faults

bool uvm_handle_fault(uint64_t fault_address, uint64_t error_code) {
    process_t* proc = current_process;
    if (fault_address < UVM_MIN) {
        // The kernel-heap range below 4 GiB: a stale TLB entry from before
        // another thread's SYS_HEAP made the page user-accessible.
        return proc && proc->privilege == PROC_PRIVILEGE_USER &&
               proc_heap_stale_fault(proc, fault_address, error_code);
    }
    if (fault_address >= UVM_MAX) return false;
    if (!proc || proc->privilege != PROC_PRIVILEGE_USER) return false;
    uvm_space_t* s = space_of(proc);
    // Kernel code can run on a program's CPU with the kernel's own page
    // tables loaded (smp_par.c); the program's memory is not there.
    if (!s || !space_is_loaded(s)) return false;

    uint64_t page = fault_address & ~(PAGE - 1);
    uint32_t need = (error_code & 2) ? UVM_PROT_WRITE
                  : (error_code & 16) ? UVM_PROT_EXEC : 0u;
    bool present = (error_code & 1) != 0;

    uint64_t irq = space_lock(s);
    uvm_area_t* a = area_at(s, page);
    bool ok = false;
    bool out_of_memory = false;
    if (a && a->prot != UVM_PROT_NONE && (a->prot & need) == need) {
        if (!present) {
            ok = frame_for(s, page, a->prot) != NULL;
            out_of_memory = !ok;
        } else {
            // The page is there and the area allows the access, so this CPU
            // used an entry that is out of date: another thread changed the
            // protection (made it more permissive) after this CPU cached it,
            // or its uvm_protect() has not got to this page yet. Bring the
            // entry up to the area's protection and run the access again.
            uint64_t* pte = walk(s, page, false);
            if (pte && (*pte & PTE_P)) {
                uint64_t want = (*pte & (PTE_ADDR | PTE_SHARED)) | leaf_flags(a->prot);
                if ((*pte & (PTE_P | PTE_W | PTE_U | PTE_NX)) != (want & (PTE_P | PTE_W | PTE_U | PTE_NX)))
                    *pte = want;
                __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
                ok = true;
            }
        }
    }
    space_unlock(s, irq);
    if (out_of_memory) {
        serial_write_str("uvm: out of memory for ");
        serial_write_str(proc->name);
        serial_write_str("\n");
    }
    return ok;
}

uint64_t uvm_committed_bytes(process_t* proc) {
    uvm_space_t* s = space_of(proc);
    return s ? s->committed_pages * PAGE : 0;
}

uint64_t uvm_mapped_bytes(process_t* proc) {
    uvm_space_t* s = space_of(proc);
    return s ? s->mapped_pages * PAGE : 0;
}

// ------------------------------------------------------------ shared memory

uvm_shm_t* uvm_shm_create(uint64_t bytes) {
    if (!bytes || bytes > (1ULL << 32)) return NULL;
    uint64_t pages = round_up(bytes) / PAGE;
    uvm_shm_t* shm = (uvm_shm_t*)alloc(sizeof(*shm));
    if (!shm) return NULL;
    void* frames = pmm_try_alloc_pages_zeroed((size_t)pages);
    if (!frames) {
        free_mem(shm);
        return NULL;
    }
    shm->refs = 1;
    shm->pages = pages;
    shm->base = (uint8_t*)frames;
    return shm;
}

uvm_shm_t* uvm_shm_get(uvm_shm_t* shm) {
    if (shm) __atomic_fetch_add(&shm->refs, 1, __ATOMIC_SEQ_CST);
    return shm;
}

void uvm_shm_put(uvm_shm_t* shm) {
    if (!shm) return;
    if (__atomic_sub_fetch(&shm->refs, 1, __ATOMIC_SEQ_CST) != 0) return;
    // Last reference: no space maps it any more (an area holds one), and
    // the shootdown of the last unmap has finished.
    for (uint64_t i = 0; i < shm->pages; i++)
        pmm_free_page_unscrubbed(shm->base + i * PAGE);
    free_mem(shm);
}

void* uvm_shm_base(uvm_shm_t* shm) { return shm ? shm->base : NULL; }
uint64_t uvm_shm_size(uvm_shm_t* shm) { return shm ? shm->pages * PAGE : 0; }

uint64_t uvm_map_shm(process_t* proc, uvm_shm_t* shm, uint64_t addr, uint32_t prot) {
    uvm_space_t* s = space_of(proc);
    if (!s || !shm) return SYS_ERR_INVAL;
    if (prot & ~(UVM_PROT_READ | UVM_PROT_WRITE)) return SYS_ERR_INVAL;   // never executable
    uint64_t len = shm->pages * PAGE;
    uvm_area_t* n = (uvm_area_t*)alloc(sizeof(*n));
    if (!n) return SYS_ERR_NOMEM;

    // The whole area is filled in under the lock, before it becomes
    // visible: a thread that touches it must never find a page without its
    // frame (the fault path would hand it a private one).
    uint64_t irq = space_lock(s);
    uint64_t from = UVM_MMAP_BASE;
    if (addr >= UVM_MIN && addr < UVM_MAX) from = round_up(addr);
    uint64_t start = find_room(s, from, len);
    if (!start && from != UVM_MMAP_BASE) start = find_room(s, UVM_MMAP_BASE, len);
    uint64_t result = start;
    if (!start) {
        result = SYS_ERR_NOMEM;
    } else {
        uint64_t flags = leaf_flags(prot) | PTE_SHARED;
        uint64_t done = 0;
        for (; done < shm->pages; done++) {
            uint64_t* pte = walk(s, start + done * PAGE, true);
            if (!pte) break;
            *pte = (uint64_t)(uintptr_t)(shm->base + done * PAGE) | flags;
        }
        if (done < shm->pages) {
            // Tables ran out: nothing was visible yet, so no flush is needed.
            for (uint64_t i = 0; i < done; i++) *walk(s, start + i * PAGE, false) = 0;
            result = SYS_ERR_NOMEM;
        } else {
            n->start = start;
            n->end = start + len;
            n->prot = prot;
            n->max_prot = prot;
            n->shm = uvm_shm_get(shm);
            insert_area(s, n);
            n = NULL;
        }
    }
    space_unlock(s, irq);
    if (n) free_mem(n);
    return result;
}
