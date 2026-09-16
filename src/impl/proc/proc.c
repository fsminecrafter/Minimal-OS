// src/impl/proc/proc.c
#include <stdint.h>
#include <stddef.h>
#include "x86_64/proc.h"
#include "x86_64/gdt.h"
#include "x86_64/scheduler.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "time.h"
#include "panic.h"
#include "x86_64/safeints.h"
#include "string.h"

extern uint64_t pml4_phys_addr;

process_t* proc_list_head = NULL;
static uint64_t pid_counter = 1;

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

		uint64_t pdpte = src_pdpt[0];
		if (pdpte & 1) {
			uint64_t* src_pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
			uint64_t* dst_pd = (uint64_t*)alloc_page_zeroed();
			if (!dst_pd) return 0;
			memcpy(dst_pd, src_pd, 512 * sizeof(uint64_t));

			dst_pdpt[0] = (uint64_t)(uintptr_t)dst_pd | (pdpte & 0xFFFULL);
		}

		dst[0] = (uint64_t)(uintptr_t)dst_pdpt | (pml4e & 0xFFFULL);
	}

	return (uint64_t)(uintptr_t)dst;
}

static int proc_map_user_pages(process_t* proc, uintptr_t address, size_t length,
					uint64_t phys_base) {
	if (!proc || length == 0) return 0;

	uintptr_t start = address & ~0xFFFULL;
	uintptr_t end = address + length;
	if (end < address) return 0;
	end = (end + 0xFFFULL) & ~0xFFFULL;

	uint64_t* pml4 = (uint64_t*)(uintptr_t)proc->pml4;
	uint64_t pml4_index = (start >> 39) & 0x1FF;
	if (((end - 1) >> 39) & 0x1FF != pml4_index || pml4_index != 0) {
		return 0;
	}

	uint64_t pml4e = pml4[pml4_index];
	if (!(pml4e & 1)) return 0;
	uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);
	pml4[pml4_index] |= 0x4;

	for (uintptr_t page = start; page < end; page += 0x1000) {
		uint64_t pdpt_index = (page >> 30) & 0x1FF;
		uint64_t pdpte = pdpt[pdpt_index];
		if (!(pdpte & 1) || (pdpte & (1ULL << 7))) return 0;
		uint64_t* pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
		pdpt[pdpt_index] |= 0x4;

		uint64_t pd_index = (page >> 21) & 0x1FF;
		uint64_t pde = pd[pd_index];
		if (!(pde & 1)) return 0;

		uint64_t* pt;
		if (pde & (1ULL << 7)) {
			pt = (uint64_t*)alloc_page_zeroed();
			if (!pt) return 0;
			uint64_t base = pde & 0xFFFFFFE00000ULL;
			uint64_t flags = pde & 0xFFFULL & ~(1ULL << 7);
			for (size_t i = 0; i < 512; i++) {
				pt[i] = (base + i * 0x1000ULL) | flags;
			}
			pd[pd_index] = (uint64_t)(uintptr_t)pt | flags;
		}
		pt = (uint64_t*)(uintptr_t)(pd[pd_index] & ~0xFFFULL);
		uint64_t pt_index = (page >> 12) & 0x1FF;
		uint64_t pte_flags = pt[pt_index] & 0xFFFULL;
		pt[pt_index] = (phys_base + (page - start)) | pte_flags | 0x4;
		pd[pd_index] |= 0x4;
	}

	return 1;
}

int proc_map_user_range(process_t* proc, void* address, size_t length) {
	if (!proc || !address || length == 0) return 0;
	uintptr_t start = (uintptr_t)address & ~0xFFFULL;
	uintptr_t end = (uintptr_t)address + length;
	if (end < (uintptr_t)address) return 0;
	uintptr_t aligned_end = (end + 0xFFFULL) & ~0xFFFULL;
	size_t pages = (aligned_end - start) / 0x1000;

	void* physical = alloc_pages_zeroed(pages);
	if (!physical) return 0;
	memcpy(physical, (void*)start, pages * 0x1000);

	if (!proc_map_user_pages(proc, (uintptr_t)address, length,
				(uint64_t)(uintptr_t)physical)) {
		free_pages(physical, pages);
		return 0;
	}

	if (!proc->user_image_phys) {
		proc->user_image_phys = physical;
		proc->user_image_pages = pages;
	} else if (!proc->user_stack_phys) {
		proc->user_stack_phys = physical;
		proc->user_stack_pages = pages;
	} else {
		free_pages(physical, pages);
		return 0;
	}
	return 1;
}

void proc_destroy_address_space(process_t* proc) {
	if (!proc || !proc->pml4) return;

	uint64_t* pml4 = (uint64_t*)(uintptr_t)proc->pml4;
	uint64_t pml4e = pml4[0];
	if (pml4e & 1) {
		uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);
		uint64_t pdpte = pdpt[0];
		if (pdpte & 1) {
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
		uint64_t user_argc, void* user_argv) {
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
		/* SysV integer args to the entry point: RDI=argc, RSI=argv.
		 * These are ordinary register writes (not popped by iretq),
		 * so they survive the ring transition below untouched. */
		"mov %[argc], %%rdi\n\t"
		"mov %[argv], %%rsi\n\t"
		"iretq\n\t"
		:
		: [pml4] "r"(pml4),
		  [ss] "r"((uint64_t)GDT_SELECTOR_DS_USER),
		  [rsp] "r"((uint64_t)user_stack_top),
		  [cs] "r"((uint64_t)GDT_SELECTOR_CS_USER),
		  [rip] "r"((uint64_t)entry_point),
		  [argc] "r"(user_argc),
		  [argv] "r"((uint64_t)(uintptr_t)user_argv)
		: "rax", "rdi", "rsi", "memory");
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
			proc_enter_ring3(self, self->entry_point, self->user_stack,
			                  self->user_argc, self->user_argv);
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
	memset_p(proc, 0, sizeof(process_t));

	proc->pid = get_next_pid();
	proc->state = PROCESS_READY;
	proc->privilege = privilege;

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

	void* stack = alloc(STACK_SIZE);
	if (!stack) {
		PANIC("Failed to allocate kernel stack");
	}
	proc->kernel_stack = (uint64_t*)((uint8_t*)stack + STACK_SIZE);

	void* user_stack = alloc(STACK_SIZE);
	if (!user_stack) {
		PANIC("Failed to allocate user stack");
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
	proc->regs[6] = (uint64_t)proc->kernel_stack;  // RSP
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

	// Places this process on some core's per-CPU ready queue - it is
	// NOT actually schedulable until this runs. See scheduler.c.
	scheduler_enqueue_new(proc);

	return proc;
}

process_t* proc_create(const char* file_name, void (*entry_point)()) {
	return proc_create_ex(file_name, entry_point, PROC_PRIVILEGE_KERNEL);
}

void kill(process_t* proc) {
	if (!proc) return;
	proc->state = PROCESS_ZOMBIE;
	// Pull it out of whatever per-CPU run queue it's sitting in so it
	// can never be dispatched again, and so the zombie-cleanup pass in
	// schedule() can safely free it later without racing a queue that
	// still points at it.
	scheduler_dequeue(proc);
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
