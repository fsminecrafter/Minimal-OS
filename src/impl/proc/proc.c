#include "x86_64/audio_streams.h"
#include "x86_64/fgpu.h"
#include "mmu_tables.h"
// src/impl/proc/proc.c
#include <stdint.h>
#include <stddef.h>
#include "x86_64/proc.h"
#include "x86_64/gdt.h"
#include "x86_64/scheduler.h"
#include "graphics.h"
#include "x86_64/window_manager.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "x86_64/uvm.h"
#include "x86_64/spinlock.h"
#include "x86_64/boot_options.h"
#include "x86_64/tlb.h"
#include "x86_64/smp.h"
#include "time.h"
#include "panic.h"
#include "x86_64/safeints.h"
#include "string.h"
#include "serial.h"

extern uint64_t pml4_phys_addr;

process_t* proc_list_head = NULL;
static uint64_t pid_counter = 1;

struct user_heap_allocation {
	void* address;
	size_t size;
	struct user_heap_allocation* next;
};

uint64_t get_next_pid() {
	return pid_counter++;
}

uint64_t get_kernel_pml4(void) {
	return pml4_phys_addr;  // Return the VALUE, not cast to pointer!
}

static uint64_t clone_kernel_pml4(void) {
	uint64_t kernel_pml4 = get_kernel_pml4();
	if (!kernel_pml4) {
		return 0;
	}

	uint64_t* src = (uint64_t*)(uintptr_t)kernel_pml4;
	uint64_t* dst = (uint64_t*)alloc_page_zeroed();
	if (!dst) {
		return 0;
	}

	memcpy(dst, src, 512 * sizeof(uint64_t));

	uint64_t pml4e = src[0];
	if (pml4e & 1) {
		uint64_t* src_pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);
		uint64_t* dst_pdpt = (uint64_t*)alloc_page_zeroed();
		if (!dst_pdpt) return 0;
		memcpy(dst_pdpt, src_pdpt, 512 * sizeof(uint64_t));

		// Every identity-mapped GiB gets its own page directory copy, so
		// user mappings (which split 2 MiB pages) never touch the
		// kernel's tables.
		for (size_t gib = 0; gib < IDENTITY_MAP_GIB; gib++) {
			uint64_t pdpte = src_pdpt[gib];
			if (!(pdpte & 1) || (pdpte & (1ULL << 7))) continue;
			uint64_t* src_pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
			uint64_t* dst_pd = (uint64_t*)alloc_page_zeroed();
			if (!dst_pd) return 0;
			memcpy(dst_pd, src_pd, 512 * sizeof(uint64_t));

			dst_pdpt[gib] = (uint64_t)(uintptr_t)dst_pd | (pdpte & 0xFFFULL);
		}

		dst[0] = (uint64_t)(uintptr_t)dst_pdpt | (pml4e & 0xFFFULL);
	}

	return (uint64_t)(uintptr_t)dst;
}

// The owner's heap lock guards its user_heap_allocations list and the
// page-table entries SYS_HEAP flips between supervisor and user access
// (several threads of a program may call SYS_HEAP on different CPUs).
static inline spinlock_t* heap_lock_of(process_t* owner) {
	return (spinlock_t*)&owner->heap_lock;
}

static int heap_map_locked(process_t* proc, void* address, size_t length);
static int heap_unmap_locked(process_t* proc, void* address, size_t length, bool* changed);

int proc_map_user_heap_range(process_t* proc, void* address, size_t length) {
	proc = proc_mem_owner(proc);
	if (!proc) return 0;
	uint64_t flags = spinlock_acquire(heap_lock_of(proc));
	int r = heap_map_locked(proc, address, length);
	spinlock_release(heap_lock_of(proc), flags);
	return r;
}

static int heap_map_locked(process_t* proc, void* address, size_t length) {
	proc = proc_mem_owner(proc);
	if (!proc || !address || length == 0) return 0;
	uintptr_t start = (uintptr_t)address & ~0xFFFULL;
	uintptr_t end = (uintptr_t)address + length;
	if (end < (uintptr_t)address || end > UINTPTR_MAX - 0xFFFULL) return 0;
	end = (end + 0xFFFULL) & ~0xFFFULL;

	uint64_t* pml4 = (uint64_t*)(uintptr_t)proc->pml4;
	uint64_t pml4_index = (start >> 39) & 0x1FF;
	if ((((end - 1) >> 39) & 0x1FF) != pml4_index || pml4_index != 0) return 0;
	uint64_t pml4e = pml4[pml4_index];
	if (!(pml4e & 1)) return 0;
	pml4[pml4_index] |= 0x4;
	uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);

	for (uintptr_t page = start; page < end; page += 0x1000) {
		uint64_t pdpt_index = (page >> 30) & 0x1FF;
		uint64_t pdpte = pdpt[pdpt_index];
		if (!(pdpte & 1) || (pdpte & (1ULL << 7))) return 0;
		pdpt[pdpt_index] |= 0x4;
		uint64_t* pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);

		uint64_t pd_index = (page >> 21) & 0x1FF;
		uint64_t pde = pd[pd_index];
		if (!(pde & 1)) return 0;
		if (pde & (1ULL << 7)) {
			uint64_t* pt = (uint64_t*)alloc_page_zeroed();
			if (!pt) return 0;
			uint64_t base = pde & 0xFFFFFFE00000ULL;
			uint64_t flags = pde & 0xFFFULL & ~(1ULL << 7);
			for (size_t i = 0; i < 512; i++) {
				pt[i] = (base + i * 0x1000ULL) | flags;
			}
			pd[pd_index] = (uint64_t)(uintptr_t)pt | flags;
		}

		uint64_t* pt = (uint64_t*)(uintptr_t)(pd[pd_index] & ~0xFFFULL);
		uint64_t pt_index = (page >> 12) & 0x1FF;
		uint64_t pte = pt[pt_index];
		if (!(pte & 1) || (pte & 0x000FFFFFFFFFF000ULL) != page) return 0;
		// User accessible, and never executable: code a program
		// generates or loads goes into a SYS_VM mapping.
		pt[pt_index] = pte | 0x4 | (1ULL << 63);
		pd[pd_index] |= 0x4;
		__asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
	}
	return 1;
}

// Takes the pages' user access away and flushes it on every CPU that has the
// program's page tables loaded (the memory goes back to the kernel heap).
int proc_unmap_user_heap_range(process_t* proc, void* address, size_t length) {
	proc = proc_mem_owner(proc);
	if (!proc) return 0;
	uint64_t flags = spinlock_acquire(heap_lock_of(proc));
	bool changed = false;
	int r = heap_unmap_locked(proc, address, length, &changed);
	if (changed) {
		uintptr_t start = (uintptr_t)address & ~0xFFFULL;
		uintptr_t end = ((uintptr_t)address + length + 0xFFFULL) & ~0xFFFULL;
		uvm_flush_range(proc, start, end);
	}
	spinlock_release(heap_lock_of(proc), flags);
	return r;
}

static int heap_unmap_locked(process_t* proc, void* address, size_t length, bool* changed) {
	proc = proc_mem_owner(proc);
	if (!proc || !address || length == 0) return 0;
	uintptr_t start = (uintptr_t)address & ~0xFFFULL;
	uintptr_t end = (uintptr_t)address + length;
	if (end < (uintptr_t)address || end > UINTPTR_MAX - 0xFFFULL) return 0;
	end = (end + 0xFFFULL) & ~0xFFFULL;
	uint64_t* pml4 = (uint64_t*)(uintptr_t)proc->pml4;
	uint64_t pml4_index = (start >> 39) & 0x1FF;
	if ((((end - 1) >> 39) & 0x1FF) != pml4_index || pml4_index != 0) return 0;
	uint64_t pml4e = pml4[pml4_index];
	if (!(pml4e & 1)) return 1;
	uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);

	for (uintptr_t page = start; page < end; page += 0x1000) {
		uint64_t pdpte = pdpt[(page >> 30) & 0x1FF];
		if (!(pdpte & 1) || (pdpte & (1ULL << 7))) continue;
		uint64_t* pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
		uint64_t pde = pd[(page >> 21) & 0x1FF];
		if (!(pde & 1) || (pde & (1ULL << 7))) continue;
		uint64_t* pt = (uint64_t*)(uintptr_t)(pde & ~0xFFFULL);
		uint64_t pt_index = (page >> 12) & 0x1FF;
		if (pt[pt_index] & 0x4ULL) *changed = true;
		pt[pt_index] &= ~(0x4ULL | (1ULL << 63));
	}
	return 1;
}

// A user-mode access to kernel-heap memory below 4 GiB faulted although the
// page is present: the CPU used a translation cached before another thread
// (on another CPU) made the page user-accessible with SYS_HEAP. Granting
// access needs no shootdown; the first use on each CPU lands here, finds the
// tables now allow it and drops the stale entry. A real violation (the
// tables still forbid it) returns false.
bool proc_heap_stale_fault(process_t* proc, uint64_t addr, uint64_t err) {
	if (!(err & 1) || !(err & 4)) return false;
	process_t* owner = proc_mem_owner(proc);
	if (!owner || !owner->pml4 || addr >= (1ULL << 39)) return false;
	uint64_t* pml4 = (uint64_t*)(uintptr_t)owner->pml4;
	uint64_t e = pml4[0];
	if ((e & 5) != 5) return false;
	uint64_t* pdpt = (uint64_t*)(uintptr_t)(e & ~0xFFFULL);
	e = pdpt[(addr >> 30) & 0x1FF];
	if ((e & 5) != 5 || (e & (1ULL << 7))) return false;
	uint64_t* pd = (uint64_t*)(uintptr_t)(e & ~0xFFFULL);
	e = pd[(addr >> 21) & 0x1FF];
	if ((e & 5) != 5 || (e & (1ULL << 7))) return false;
	uint64_t* pt = (uint64_t*)(uintptr_t)(e & ~0xFFFULL);
	e = pt[(addr >> 12) & 0x1FF];
	if ((e & 5) != 5) return false;
	if ((err & 2) && !(e & 2)) return false;
	if ((err & 16) && (e & (1ULL << 63))) return false;
	__asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
	return true;
}

static int proc_user_heap_valid_range(void* address, size_t size) {
	uintptr_t start = (uintptr_t)address;
	if (!address || start < 64 || size == 0 || size > SIZE_MAX - 128) return 0;
	uintptr_t available = UINTPTR_MAX - start;
	if (size > available || 64 > available - size) return 0;
	uintptr_t end = start + size + 64;
	return end <= UINTPTR_MAX - 0xFFFULL;
}

// Caller holds the heap lock.
static void proc_user_heap_unmap_unused_pages(process_t* proc,
						       uintptr_t start,
						       uintptr_t end) {
	uintptr_t first_page = start & ~0xFFFULL;
	uintptr_t end_page = (end + 0xFFFULL) & ~0xFFFULL;
	bool changed = false;
	uintptr_t lo = (uintptr_t)-1, hi = 0;
	for (uintptr_t page = first_page; page < end_page; page += 0x1000) {
		bool still_owned = false;
		for (struct user_heap_allocation* item = proc->user_heap_allocations;
		     item; item = item->next) {
			uintptr_t item_start = (uintptr_t)item->address - 64;
			uintptr_t item_end = (uintptr_t)item->address + item->size + 64;
			if (item_start < page + 0x1000 && item_end > page) {
				still_owned = true;
				break;
			}
		}
		if (!still_owned) {
			bool page_changed = false;
			heap_unmap_locked(proc, (void*)page, 0x1000, &page_changed);
			if (page_changed) {
				changed = true;
				if (page < lo) lo = page;
				if (page + 0x1000 > hi) hi = page + 0x1000;
			}
		}
	}
	// One flush for the whole range, before the memory is reused.
	if (changed) uvm_flush_range(proc, lo, hi);
}

static int heap_contains_locked(process_t* proc, void* address) {
	for (struct user_heap_allocation* item = proc->user_heap_allocations;
	     item; item = item->next) {
		if (item->address == address) return 1;
	}
	return 0;
}

int proc_user_heap_contains(process_t* proc, void* address) {
	proc = proc_mem_owner(proc);
	if (!proc || !address) return 0;
	uint64_t flags = spinlock_acquire(heap_lock_of(proc));
	int r = heap_contains_locked(proc, address);
	spinlock_release(heap_lock_of(proc), flags);
	return r;
}

int proc_user_heap_contains_range(process_t* proc, const void* address,
				  size_t length) {
	proc = proc_mem_owner(proc);
	if (!proc || !address || !length) return 0;
	uintptr_t start = (uintptr_t)address;
	if (length > UINTPTR_MAX - start) return 0;
	uintptr_t end = start + length;
	int found = 0;
	uint64_t flags = spinlock_acquire(heap_lock_of(proc));
	for (struct user_heap_allocation* item = proc->user_heap_allocations;
	     item; item = item->next) {
		uintptr_t allocation_start = (uintptr_t)item->address;
		if (item->size > UINTPTR_MAX - allocation_start) continue;
		uintptr_t allocation_end = allocation_start + item->size;
		if (start >= allocation_start && end <= allocation_end) { found = 1; break; }
	}
	spinlock_release(heap_lock_of(proc), flags);
	return found;
}

int proc_user_range_ok(process_t* proc, const void* address,
		       size_t length, bool write) {
	proc = proc_mem_owner(proc);
	if (proc_user_heap_contains_range(proc, address, length)) return 1;
	// Image, stacks and SYS_VM mappings: uvm.c knows their protection.
	return uvm_range_ok(proc, address, length, write) ? 1 : 0;
}

int proc_user_range_contains(process_t* proc, const void* address,
			     size_t length) {
	proc = proc_mem_owner(proc);
	if (proc_user_heap_contains_range(proc, address, length)) return 1;
	if (uvm_range_ok(proc, address, length, false)) return 1;
	if (!proc || !address || !length) return 0;
	uintptr_t start = (uintptr_t)address;
	if (length > UINTPTR_MAX - start) return 0;
	uintptr_t end = start + length;
	if (proc->user_image_size && proc->user_image_size <=
	    UINTPTR_MAX - proc->user_image_address &&
	    start >= proc->user_image_address &&
	    end <= proc->user_image_address + proc->user_image_size) return 1;
	if (proc->user_stack_size && proc->user_stack_size <=
	    UINTPTR_MAX - proc->user_stack_address &&
	    start >= proc->user_stack_address &&
	    end <= proc->user_stack_address + proc->user_stack_size) return 1;
	return 0;
}

int proc_user_heap_track(process_t* proc, void* address, size_t size) {
	proc = proc_mem_owner(proc);
	if (!proc || !proc_user_heap_valid_range(address, size)) return 0;
	struct user_heap_allocation* item = alloc(sizeof(*item));
	if (!item) return 0;
	item->address = address;
	item->size = size;
	uint64_t flags = spinlock_acquire(heap_lock_of(proc));
	if (heap_contains_locked(proc, address)) {
		spinlock_release(heap_lock_of(proc), flags);
		free_mem(item);
		return 0;
	}
	item->next = proc->user_heap_allocations;
	proc->user_heap_allocations = item;
	spinlock_release(heap_lock_of(proc), flags);
	return 1;
}

int proc_user_heap_untrack(process_t* proc, void* address) {
	proc = proc_mem_owner(proc);
	if (!proc || !address) return 0;
	uint64_t flags = spinlock_acquire(heap_lock_of(proc));
	struct user_heap_allocation** link = &proc->user_heap_allocations;
	while (*link && (*link)->address != address) link = &(*link)->next;
	if (!*link) {
		spinlock_release(heap_lock_of(proc), flags);
		return 0;
	}
	struct user_heap_allocation* item = *link;
	*link = item->next;
	uintptr_t start = (uintptr_t)item->address - 64;
	uintptr_t end = (uintptr_t)item->address + item->size + 64;
	proc_user_heap_unmap_unused_pages(proc, start, end);
	spinlock_release(heap_lock_of(proc), flags);
	free_mem(item);
	return 1;
}

int proc_user_heap_resize(process_t* proc, void* old_address,
			  void* new_address, size_t new_size) {
	proc = proc_mem_owner(proc);
	if (!proc || !proc_user_heap_valid_range(new_address, new_size)) return 0;
	if (!old_address) return proc_user_heap_track(proc, new_address, new_size);
	uint64_t flags = spinlock_acquire(heap_lock_of(proc));
	for (struct user_heap_allocation* item = proc->user_heap_allocations;
	     item; item = item->next) {
		if (item->address != old_address) continue;
		if (new_address != old_address &&
		    heap_contains_locked(proc, new_address)) break;
		uintptr_t old_start = (uintptr_t)item->address - 64;
		uintptr_t old_end = (uintptr_t)item->address + item->size + 64;
		item->address = new_address;
		item->size = new_size;
		proc_user_heap_unmap_unused_pages(proc, old_start, old_end);
		spinlock_release(heap_lock_of(proc), flags);
		return 1;
	}
	spinlock_release(heap_lock_of(proc), flags);
	return 0;
}

void proc_user_heap_cleanup(process_t* proc) {
	// A thread's allocations belong to its owner, freed with the owner.
	if (!proc || proc->mem_owner) return;
	struct user_heap_allocation* item = proc->user_heap_allocations;
	uint64_t allocation_count = 0;
	uint64_t reclaimed_bytes = 0;
	while (item) {
		struct user_heap_allocation* next = item->next;
		size_t item_size = item->size;
		free_mem(item->address);
		free_mem(item);
		allocation_count++;
		if (UINT64_MAX - reclaimed_bytes < item_size) {
			reclaimed_bytes = UINT64_MAX;
		} else {
			reclaimed_bytes += item_size;
		}
		item = next;
	}
	proc->user_heap_allocations = NULL;
	if (allocation_count) {
		serial_write_str("[GC] reclaimed ");
		serial_write_dec(reclaimed_bytes);
		serial_write_str(" bytes in ");
		serial_write_dec(allocation_count);
		serial_write_str(" allocations\n");
	}
}

void proc_destroy_address_space(process_t* proc) {
	if (!proc || !proc->pml4) return;
	// A thread runs in its owner's page tables: never free them here.
	if (proc->mem_owner) {
		proc->pml4 = 0;
		return;
	}

	uvm_destroy(proc);

	uint64_t* pml4 = (uint64_t*)(uintptr_t)proc->pml4;
	uint64_t pml4e = pml4[0];
	if (pml4e & 1) {
		uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);
		for (size_t gib = 0; gib < IDENTITY_MAP_GIB; gib++) {
			uint64_t pdpte = pdpt[gib];
			if (!(pdpte & 1) || (pdpte & (1ULL << 7))) continue;
			uint64_t* pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
			for (size_t i = 0; i < 512; i++) {
				if ((pd[i] & 1) && !(pd[i] & (1ULL << 7))) {
					free_page((void*)(uintptr_t)(pd[i] & ~0xFFFULL));
				}
			}
			free_page(pd);
		}
		free_page(pdpt);
	}
	free_page(pml4);
	proc->pml4 = 0;
}

static void proc_enter_ring3(process_t* self,
		void (*entry_point)(), void* user_stack_top,
		uint64_t user_argc, void* user_argv, void* user_envp) {
	uint64_t pml4 = self->pml4;
	__asm__ volatile(
		"mov %[pml4], %%rax\n\t"
		"mov %%rax, %%cr3\n\t"
		"pushq %[ss]\n\t"
		"pushq %[rsp]\n\t"
		"pushfq\n\t"
		"popq %%rax\n\t"
		"orq $0x200, %%rax\n\t"
		"pushq %%rax\n\t"
		"pushq %[cs]\n\t"
		"pushq %[rip]\n\t"
		/* SysV integer args to the entry point: RDI=argc, RSI=argv,
		 * RDX=envp. These are ordinary register writes (not popped by
		 * iretq), so they survive the ring transition below untouched. */
		"mov %[argc], %%rdi\n\t"
		"mov %[argv], %%rsi\n\t"
		"mov %[envp], %%rdx\n\t"
		"iretq\n\t"
		:
		: [pml4] "r"(pml4),
		  [ss] "r"((uint64_t)GDT_SELECTOR_DS_USER),
		  [rsp] "r"((uint64_t)user_stack_top),
		  [cs] "r"((uint64_t)GDT_SELECTOR_CS_USER),
		  [rip] "r"((uint64_t)entry_point),
		  [argc] "r"(user_argc),
		  [argv] "r"((uint64_t)(uintptr_t)user_argv),
		  [envp] "r"((uint64_t)(uintptr_t)user_envp)
		: "rax", "rdi", "rsi", "rdx", "memory");
	for (;;) {
		asm volatile("cli; hlt" ::: "memory");
	}
}

/*
 * proc_trampoline
 *
 * SAFETY NET for process entry points.
 *
 * context_switch.asm performs a raw `jmp` (not `call`) into a brand new
 * process's saved RIP the very first time that process ever runs.
 * Because it's a jmp, no return address is ever pushed onto that
 * process's fresh stack. That stack comes from alloc(STACK_SIZE),
 * which zero-fills memory, so the very top of a never-before-run
 * process's stack is 0x0000000000000000.
 *
 * If a process's entry function ever executes a plain `ret` - falls
 * off the end, or does a bare `return;`, instead of looping forever or
 * calling process_exit()/kill() itself - that `ret` pops zero as the
 * return address and jumps to address 0. In this kernel's memory
 * layout, address 0 sits inside the identity-mapped, present,
 * writable, executable low-memory region set up in main.asm, so this
 * is NOT a page fault: the CPU just starts executing whatever raw
 * bytes happen to be there as machine code. That's undefined behaviour
 * that can hang or corrupt the system anywhere, often with no obvious
 * link back to the process that actually caused it.
 *
 * To make that impossible, every process's saved entry RIP is now
 * this trampoline instead of the caller's real entry point. The
 * trampoline calls the real entry point through a normal `call`
 * (which *does* push a valid return address), and if that function
 * ever returns, terminates the process safely through process_exit()
 * instead of falling through to a garbage jump.
 */
static void proc_trampoline(void) {
	process_t* self = current_process;

	if (self && self->entry_point) {
		if (self->privilege == PROC_PRIVILEGE_USER) {
			serial_write_str("PROC: entering user process\n");
			proc_enter_ring3(self, self->entry_point, self->user_stack,
			                  self->user_argc, self->user_argv, self->user_envp);
		}
		self->entry_point();
	}

	/* The entry point returned instead of looping forever or exiting
	 * itself. Terminate this process the correct way. */
	process_exit();

	for (;;) {
		schedule();
		asm volatile("sti; hlt" ::: "memory");
	}
}

process_t* proc_create_ex(const char* file_name, void (*entry_point)(),
                          process_privilege_t privilege) {
	process_t* proc = alloc(sizeof(process_t));
	if (!proc) {
		PANIC("Failed to allocate memory for process_t");
	}
	memset(proc, 0, sizeof(process_t));

	proc->pid = get_next_pid();
	proc->state = PROCESS_READY;
	proc->privilege = privilege;
	proc->parent_pid = current_process ? current_process->pid : 0;
	proc->is_window_manager =
		privilege == PROC_PRIVILEGE_USER && file_name &&
		current_process &&
		current_process->privilege == PROC_PRIVILEGE_KERNEL &&
		strcmp(file_name, "0:/programs/mosWM/mosWM.run") == 0;

	// Initialize timing fields
	proc->wake_time_ms = 0;
	proc->cpu_time_ms = 0;
	proc->creation_time_ms = time_get_uptime_ms();

	// === BEGIN name construction ===
	char* p = proc->name;

	// 1. Copy file name
	if (file_name) {
		while (*file_name && (p - proc->name) < MAX_PROCESS_NAME_LEN - 1) {
			*p++ = *file_name++;
		}
	} else {
		const char* fallback = "unknown";
		while (*fallback && (p - proc->name) < MAX_PROCESS_NAME_LEN - 1) {
			*p++ = *fallback++;
		}
	}

	// 2. Append '@'
	if ((p - proc->name) < MAX_PROCESS_NAME_LEN - 1) *p++ = '@';

	// 3. Append hex address
	char hex[17];
	hex_to_str((uint64_t)entry_point, hex);
	const char* h = hex;
	while (*h && (p - proc->name) < MAX_PROCESS_NAME_LEN - 1) {
		*p++ = *h++;
	}

	// 4. Append " [pid="
	const char* pidprefix = " [pid=";
	while (*pidprefix && (p - proc->name) < MAX_PROCESS_NAME_LEN - 1) {
		*p++ = *pidprefix++;
	}

	// 5. Append PID in decimal
	char dec[21];
	uint_to_str(proc->pid, dec);
	const char* d = dec;
	while (*d && (p - proc->name) < MAX_PROCESS_NAME_LEN - 1) {
		*p++ = *d++;
	}

	// 6. Append ']'
	if ((p - proc->name) < MAX_PROCESS_NAME_LEN - 1) *p++ = ']';

	// Null terminate
	*p = '\0';
	// === END name construction ===

	// Every process gets its own PML4 root, cloned from the kernel's
	// current page tables so kernel mappings remain available while
	// still preventing a process from reusing the exact same CR3 as
	// every other process. This is the minimum step toward real address-
	// space isolation; the kernel still runs at CPL0 and user code is
	// not yet switched through a ring3 path.
	proc->pml4 = (privilege == PROC_PRIVILEGE_USER)
		? clone_kernel_pml4()
		: get_kernel_pml4();
	if (!proc->pml4) {
		PANIC("Failed to create PML4 for new process");
	}
	if (privilege == PROC_PRIVILEGE_USER && !uvm_create(proc)) {
		PANIC("Failed to create the address space of a new process");
	}

	void* stack = alloc(KERNEL_STACK_SIZE);
	if (!stack) {
		proc->kernel_stack_pages = (KERNEL_STACK_SIZE + 0xFFFu) / 0x1000u;
		stack = alloc_pages_zeroed(proc->kernel_stack_pages);
		if (!stack) PANIC("Failed to allocate kernel stack");
		proc->kernel_stack_page_backed = true;
	}
	proc->kernel_stack = (uint64_t*)((uint8_t*)stack + KERNEL_STACK_SIZE);

	void* user_stack = alloc(STACK_SIZE);
	if (!user_stack) {
		user_stack = alloc_pages_zeroed((STACK_SIZE + 0xFFFu) / 0x1000u);
		if (!user_stack) PANIC("Failed to allocate user stack");
	}
	proc->user_stack = (uint64_t*)((uint8_t*)user_stack + STACK_SIZE);

	// Zero all registers first
	for (int i = 0; i < 9; i++) {
		proc->regs[i] = 0;
	}

	// Kernel processes call this entry through the trampoline. User
	// processes use it as the actual ring3 entry target.
	proc->entry_point = entry_point;

	// Then set the important ones
	// RSP. The trampoline is entered with a jmp, so make it look like a
	// call happened: 16-byte aligned minus the return address slot. GCC
	// emits aligned SSE stores (movaps) to the stack.
	proc->regs[6] = (((uint64_t)proc->kernel_stack) & ~0xFULL) - 8;
	proc->regs[7] = (uint64_t)proc_trampoline;     // RIP
	proc->regs[8] = 0x202;                         // RFLAGS (IF=1, reserved bit 1)

	// proc->sched_level/sched_cpu/sched_ticks_used/rq_next are already
	// zeroed by memset_p() above; scheduler_enqueue_new() below sets
	// them properly and is what actually makes this process visible to
	// any core's dispatch loop.

	/*
	 * proc_list_head is the same global list schedule()'s
	 * zombie/terminated cleanup pass walks and mutates. Protected
	 * with the real cross-core scheduler_lock() (spinlock.h) instead
	 * of plain irq_save() - cli() alone doesn't stop a second physical
	 * core from touching this list at the same time.
	 */
	uint64_t proc_list_flags = scheduler_lock();
	proc->next = proc_list_head;
	proc_list_head = proc;
	scheduler_unlock(proc_list_flags);
	irq_restore(proc_list_flags, __FILE__, __func__, __LINE__);

	// A user process has no image or stack mapped into its address space
	// yet. Queueing it now would let another core dispatch it into the
	// kernel's supervisor-only identity mapping before the caller finishes
	// setting up its address space (runcommand.c), so it stays parked
	// until proc_start().
	if (privilege == PROC_PRIVILEGE_USER) {
		proc->state = PROCESS_PAUSED;
		return proc;
	}

	// Places this process on some core's per-CPU ready queue - it is
	// NOT actually schedulable until this runs. See scheduler.c.
	scheduler_enqueue_new(proc);

	return proc;
}

void proc_start(process_t* proc) {
	if (!proc || proc->state != PROCESS_PAUSED) return;
	proc->state = PROCESS_READY;
	scheduler_enqueue_new(proc);
}

process_t* proc_create(const char* file_name, void (*entry_point)()) {
	return proc_create_ex(file_name, entry_point, PROC_PRIVILEGE_KERNEL);
}

void kill(process_t* proc) {
	if (!proc) return;
	bool was_live = proc->state != PROCESS_ZOMBIE && proc->state != PROCESS_TERMINATED;
	if (!proc->mem_owner && proc->thread_count) {
		/* Off every run queue first (stops proc_kill_threads recursing into it),
		 * then its threads, which may be running on other CPUs. */
		scheduler_mark_dead(proc, PROCESS_ZOMBIE);
		proc_kill_threads(proc);
	}
	window_manager_process_exit(proc->pid);
	fgpu_process_exit(proc->pid);
	audio_streams_on_process_exit(proc->pid);
	graphics_release_screen_for_pid(proc->pid);
	if (was_live && !proc->crashed)     /* ended because one of its threads crashed */
		proc->killed = true;            /* SYS_WAIT reports it as killed */
	// ZOMBIE, off whatever run queue it sits in (it can never be dispatched
	// again, so the cleanup pass in schedule() can free it later), and a CPU
	// that is running it right now is told to leave it.
	scheduler_mark_dead(proc, PROCESS_ZOMBIE);
}

/* ------------------------------------------------------- user threads */

uint64_t proc_create_thread(process_t* owner, uint64_t entry,
			    uint64_t stack_top, uint64_t arg) {
	return proc_create_thread_ex(owner, entry, stack_top, arg, 0, 0);
}

uint64_t proc_create_thread_ex(process_t* owner, uint64_t entry,
			       uint64_t stack_top, uint64_t arg,
			       uint64_t fs_base, uint64_t exit_futex) {
	owner = proc_mem_owner(owner);
	if (!owner || owner->privilege != PROC_PRIVILEGE_USER || !owner->pml4 ||
	    owner->state == PROCESS_ZOMBIE || owner->state == PROCESS_TERMINATED)
		return 0;
	// FS base must be a canonical lower-half address (WRMSR faults on
	// anything else); the exit futex an aligned word the program owns.
	if (fs_base >= 0x0000800000000000ULL) return 0;
	if (exit_futex && ((exit_futex & 3u) ||
	    !proc_user_range_contains(owner, (const void*)(uintptr_t)exit_futex, 4)))
		return 0;
	// The code must be executable memory of the program (its image or a
	// SYS_VM mapping with SYS_VM_PROT_EXEC), the stack (its top 256 bytes
	// at least) memory the program owns.
	if (!uvm_is_executable(owner, entry))
		return 0;
	stack_top &= ~0xFULL;
	if (stack_top < 0x1000 + 256 ||
	    !proc_user_range_contains(owner, (const void*)(uintptr_t)(stack_top - 256), 256))
		return 0;

	char name[MAX_PROCESS_NAME_LEN];
	size_t n = 0;
	for (const char* s = owner->name; *s && *s != '@' && n < sizeof(name) - 8; s++) name[n++] = *s;
	const char* suffix = "#thread";
	while (*suffix && n < sizeof(name) - 1) name[n++] = *suffix++;
	name[n] = 0;

	process_t* t = proc_create_ex(name, (void (*)())(uintptr_t)entry, PROC_PRIVILEGE_USER);
	if (!t) return 0;
	uint64_t tid = t->pid;
	// proc_create_ex() gave it page tables of its own; it uses the owner's.
	proc_destroy_address_space(t);          /* mem_owner not set yet: frees them */
	// Joins the group, unless the program is already ending (its exit has
	// killed or is killing the threads: one created now would outlive it).
	uint64_t gflags = scheduler_lock();
	if (__atomic_load_n(&owner->group_dying, __ATOMIC_SEQ_CST) ||
	    owner->state == PROCESS_ZOMBIE || owner->state == PROCESS_TERMINATED) {
		scheduler_unlock(gflags);
		t->state = PROCESS_ZOMBIE;          /* never started; reaped, no address space */
		return 0;
	}
	t->mem_owner = owner;
	t->pml4 = owner->pml4;
	owner->thread_count++;
	scheduler_unlock(gflags);
	t->parent_pid = owner->pid;
	process_t* creator = current_process;
	if (creator && proc_mem_owner(creator) == owner) {
		t->sched_prio = creator->sched_prio;    /* inherits priority and affinity */
		t->affinity = creator->affinity;
	}
	// SysV: RSP % 16 == 8 at the entry, as if called.
	t->user_stack = (uint64_t*)(uintptr_t)(stack_top - 8);
	t->user_argc = arg;                     /* RDI: the thread's argument */
	t->user_argv = NULL;
	t->user_envp = NULL;
	t->fs_base = fs_base;                   /* loaded when it is switched to */
	t->exit_futex = exit_futex;
	// For crash reports (the image is the owner's; nothing to free here).
	t->user_image_address = owner->user_image_address;
	t->user_image_size = owner->user_image_size;
	t->cleanup_entry = NULL;

	// Spread over the CPUs like a process (or kept on the owner's CPU with
	// the 'threads1cpu' boot flag); it may be killed before it starts.
	scheduler_start_thread(t, owner);
	return tid;                             /* t may be gone already */
}

// Marks every thread of `owner` dead (not the calling thread) and tells the
// CPUs running them to stop. The list is walked under the scheduler lock
// (other CPUs reap processes from it); only pids leave the lock.
void proc_kill_threads(process_t* owner) {
	if (!owner || owner->mem_owner || !owner->thread_count) return;
	for (;;) {
		uint64_t pids[32];
		uint32_t n = 0;
		uint64_t flags = scheduler_lock();
		for (process_t* p = proc_list_head; p && n < 32; p = p->next) {
			if (p->mem_owner == owner && p != current_process &&
			    p->state != PROCESS_ZOMBIE && p->state != PROCESS_TERMINATED) {
				p->killed = true;
				scheduler_mark_dead(p, PROCESS_ZOMBIE);
				pids[n++] = p->pid;
			}
		}
		scheduler_unlock(flags);
		for (uint32_t i = 0; i < n; i++) {
			window_manager_process_exit(pids[i]);
			fgpu_process_exit(pids[i]);
			audio_streams_on_process_exit(pids[i]);
			graphics_release_screen_for_pid(pids[i]);
		}
		if (n < 32) break;
	}
}

// Waits until no thread of `owner` is on a CPU any more (they were marked
// dead: each leaves at its next interrupt, which the kick brings at once).
// Needed before the owner frees its address space. Keeps answering TLB
// shootdowns meanwhile.
void proc_wait_threads_off_cpu(process_t* owner) {
	if (!owner || owner->mem_owner || !owner->thread_count) return;
	uint64_t began = time_get_uptime_ms();
	bool warned = false;
	for (;;) {
		bool busy = false;
		uint64_t flags = scheduler_lock();
		for (process_t* p = proc_list_head; p; p = p->next) {
			if (p->mem_owner != owner || p == current_process) continue;
			if (p->on_cpu) {
				busy = true;
				scheduler_kick(p);
			}
		}
		scheduler_unlock(flags);
		if (!busy) return;
		for (int i = 0; i < 200; i++) {
			tlb_poll();
			__asm__ volatile("pause" ::: "memory");
		}
		if (!warned && time_get_uptime_ms() - began > 3000) {
			warned = true;
			serial_write_str("[PROC] a thread did not leave its CPU: ");
			serial_write_str(owner->name);
			serial_write_str("\n");
		}
	}
}

void proc_kill_group(process_t* p) {
	process_t* owner = proc_mem_owner(p);
	if (!owner) return;
	proc_kill_threads(owner);
	if (owner != current_process && owner->state != PROCESS_ZOMBIE &&
	    owner->state != PROCESS_TERMINATED)
		kill(owner);
}

process_t* get_proc_by_name(const char* name) {
	for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
		if (!name) continue;

		const char* a = proc->name;
		const char* b = name;
		while (*a && *b && *a == *b) {
			a++; b++;
		}

		if (*a == '\0' && *b == '\0') {
			return proc;
		}
	}
	return NULL;
}

// Allocates an array of process pointers. Caller must free it.
process_t** get_procs(size_t* count) {
	size_t n = 0;
	for (process_t* p = proc_list_head; p != NULL; p = p->next) {
		n++;
	}

	if (count) *count = n;
	if (n == 0) return NULL;

	process_t** list = alloc(sizeof(process_t*) * n);
	if (!list) {
		if (count) *count = 0;
		return NULL;
	}

	size_t i = 0;
	for (process_t* p = proc_list_head; p != NULL; p = p->next) {
		list[i++] = p;
	}

	return list;
}
