#ifndef UVM_H
#define UVM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * User virtual memory: the part of a program's address space above the
 * kernel's identity-mapped 4 GiB.
 *
 * A program's image, its main stack, its argument block and everything it
 * maps with SYS_VM live here, as a sorted list of areas with a protection
 * each. Pages get a frame from the page pool (pmm.h) the first time they
 * are touched and read as zero until written; an area that was never
 * touched costs no memory. SYS_HEAP memory is separate: it stays on the
 * kernel heap below 4 GiB.
 *
 * Threads share their owner's space; every function here takes any member
 * of the group. A group runs on one CPU, so page-table changes are only
 * flushed on the CPU that makes them.
 */

struct process;

// The layout. PML4 slots 1..255 belong to the program (512 GiB each).
#define UVM_MIN         0x0000008000000000ULL   // 512 GiB: nothing below it
#define UVM_IMAGE_BASE  0x0000010000000000ULL   // 1 TiB: the program image
#define UVM_MMAP_BASE   0x0000020000000000ULL   // 2 TiB: SYS_VM_MAP without an address
#define UVM_STACK_TOP   0x00007FFFF0000000ULL   // the main stack ends here
#define UVM_ARGS_BASE   0x00007FFFF8000000ULL   // argv/envp block (argblock.h)
#define UVM_MAX         0x00007FFFFFE00000ULL

#define UVM_PAGE_SIZE   0x1000ULL

// Protection: the SYS_VM_PROT_* bits of syscall.h.
#define UVM_PROT_NONE  0u
#define UVM_PROT_READ  1u
#define UVM_PROT_WRITE 2u
#define UVM_PROT_EXEC  4u

// uvm_map flags (SYS_VM_MAP_* in syscall.h).
#define UVM_MAP_FIXED    (1u << 0)   // exactly at `addr`, or fail
#define UVM_MAP_POPULATE (1u << 1)   // give every page its frame now

// Sets up an empty space for a new user process (its page tables exist).
bool uvm_create(struct process* proc);
// Frees every frame, page table and area. Owner only; a thread is ignored.
void uvm_destroy(struct process* proc);

// Maps `len` bytes (rounded up to pages). Without UVM_MAP_FIXED the kernel
// picks the address, at or above `addr` when it is given, and leaves an
// unmapped page on both sides. Returns the address or a SYS_ERR_* value.
uint64_t uvm_map(struct process* proc, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags);
// Removes whole pages of [addr, addr+len); pages that are not mapped are
// skipped. Returns SYS_SUCCESS or SYS_ERR_INVAL.
uint64_t uvm_unmap(struct process* proc, uint64_t addr, uint64_t len);
// Changes the protection of [addr, addr+len), all of which must be mapped.
// Contents are kept, also while the protection is UVM_PROT_NONE.
uint64_t uvm_protect(struct process* proc, uint64_t addr, uint64_t len, uint32_t prot);
// Gives the frames of [addr, addr+len) back; the pages stay mapped and
// read as zero again.
uint64_t uvm_decommit(struct process* proc, uint64_t addr, uint64_t len);

// True when [ptr, ptr+len) is mapped readable (and writable, for `write`).
// Also gives every page of it a frame, so kernel code that goes on to use
// the range cannot run out of memory half way: false when it cannot.
bool uvm_range_ok(struct process* proc, const void* ptr, size_t len, bool write);
// True when `addr` lies in an executable area.
bool uvm_is_executable(struct process* proc, uint64_t addr);

// Copy into / out of / clear a space from kernel code, whatever page
// tables are loaded and whatever the areas' protection. The range must be
// mapped. False when it is not, or when memory runs out.
bool uvm_copy_in(struct process* proc, uint64_t addr, const void* src, size_t len);
bool uvm_copy_out(struct process* proc, uint64_t addr, void* dst, size_t len);

// Page-fault entry (isr_handlers.c). True when the fault was a first touch
// of a mapped page and is now resolved: the instruction can run again.
bool uvm_handle_fault(uint64_t fault_address, uint64_t error_code);

// Bytes of frames a space holds, and bytes it has mapped.
uint64_t uvm_committed_bytes(struct process* proc);
uint64_t uvm_mapped_bytes(struct process* proc);

#endif // UVM_H
