#include <stdint.h>
#include <stddef.h>
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/allocator.h"
#include "time.h"
#include "panic.h"
#include "x86_64/safeints.h"

extern uint64_t pml4_phys_addr;

#define STACK_SIZE 0x1000  // 4 KB

process_t* proc_list_head = NULL;
static uint64_t pid_counter = 1;

uint64_t get_next_pid() {
	return pid_counter++;
}

uint64_t get_kernel_pml4(void) {
	return pml4_phys_addr;  // Return the VALUE, not cast to pointer!
}

void memset_p(void* dest, uint8_t val, uint64_t len) { //changed memset to memset_p to make compiler STAY SILENT
	uint8_t* ptr = dest;
	for (uint64_t i = 0; i < len; i++) {
		ptr[i] = val;
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
		self->entry_point();
	}

	/* The entry point returned instead of looping forever or exiting
	 * itself. Terminate this process the correct way. */
	process_exit();

	/*
	 * process_exit() marks this process TERMINATED and calls
	 * schedule(), which normally switches away and never returns here.
	 * The only way control could fall through to this point is if, at
	 * the exact moment of exit, there was no other READY process for
	 * the scheduler to pick (schedule() just returns in that case
	 * instead of switching). That shouldn't happen in practice - there
	 * is always at least one other background process alive - but if
	 * it ever does, keep retrying the scheduler instead of running off
	 * the end of this function. `sti` before each `hlt` guarantees
	 * interrupts are enabled so a timer tick can always wake us up to
	 * try again.
	 */
	for (;;) {
		schedule();
		asm volatile("sti; hlt" ::: "memory");
	}
}

process_t* proc_create(const char* file_name, void (*entry_point)()) {
	process_t* proc = alloc(sizeof(process_t));
	if (!proc) {
		PANIC("Failed to allocate memory for process_t");
	}
	memset_p(proc, 0, sizeof(process_t));

	proc->pid = get_next_pid();
	proc->state = PROCESS_READY;

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

	// Remaining setup
	proc->pml4 = (uint64_t)get_kernel_pml4();
	if (!proc->pml4) {
		PANIC("Failed to get PML4 for new process");
	}

	void* stack = alloc(STACK_SIZE);
	if (!stack) {
		PANIC("Failed to allocate kernel stack");
	}
	proc->kernel_stack = (uint64_t*)((uint8_t*)stack + STACK_SIZE);

	// Zero all registers first
	for (int i = 0; i < 9; i++) {
		proc->regs[i] = 0;
	}

	// Remember the real entry point so proc_trampoline() can call it,
	// and point the saved RIP at the trampoline rather than directly
	// at the caller's function - see proc_trampoline()'s comment for
	// the full reasoning.
	proc->entry_point = entry_point;

	// Then set the important ones
	proc->regs[6] = (uint64_t)proc->kernel_stack;  // RSP
	proc->regs[7] = (uint64_t)proc_trampoline;     // RIP
	proc->regs[8] = 0x202;                         // RFLAGS (IF=1, reserved bit 1)

	/*
	 * proc_list_head is the same global list schedule()'s
	 * zombie/terminated cleanup pass walks and mutates (see the long
	 * comment on schedule() in scheduler.c). Prepending here is a
	 * read-modify-write of proc_list_head + proc->next; if a PIT tick
	 * preempts createProcess() between these two lines with a
	 * half-updated list, a concurrent schedule() call could walk a
	 * torn list. Protect it the same way, with irq_save()/irq_restore()
	 * - a no-op if we're already in interrupt context, and a real
	 * (brief) critical section if called from process context.
	 */
	uint64_t proc_list_flags = irq_save(__FILE__, __func__, __LINE__);
	proc->next = proc_list_head;
	proc_list_head = proc;
	irq_restore(proc_list_flags, __FILE__, __func__, __LINE__);

	return proc;
}

void kill(process_t* proc) {
	if (!proc) return;
	proc->state = PROCESS_ZOMBIE;
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
