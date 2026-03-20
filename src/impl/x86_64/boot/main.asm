global start
global multiboot_info_addr
global pml4_phys_addr
extern long_mode_start

global tss_struct
global double_fault_stack
global double_fault_stack_top
global kernel_stack_top
global kernel_stack

global page_table_l4
global page_table_l3
global page_table_l2
global page_table_l3_mmio
global page_table_l2_mmio

global idt_descriptor64

section .bss
    align 8
multiboot_info_addr:    resq 1
pml4_phys_addr:         resq 1

    ; a 4 KiB stack for double‑fault IST1
    align 16
double_fault_stack:     resb 4096
double_fault_stack_top:
    align 16
kernel_stack:
    resb 0x40000
kernel_stack_top:


section .text
bits 32
start:
	mov [multiboot_info_addr], ebx

	mov esp, stack_top
	call check_multiboot
	call check_cpuid
	call check_long_mode

	call setup_page_tables
	call enable_paging

	lgdt [gdt64.pointer]
	jmp gdt64.code_segment:long_mode_start

	hlt

check_multiboot:
	cmp eax, 0x36d76289
	jne .no_multiboot
	ret
.no_multiboot:
	mov al, "M"
	jmp error

check_cpuid:
	pushfd
	pop eax
	mov ecx, eax
	xor eax, 1 << 21
	push eax
	popfd
	pushfd
	pop eax
	push ecx
	popfd
	cmp eax, ecx
	je .no_cpuid
	ret
.no_cpuid:
	mov al, "C"
	jmp error

check_long_mode:
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001
	jb .no_long_mode

	mov eax, 0x80000001
	cpuid
	test edx, 1 << 29
	jz .no_long_mode
	
	ret
.no_long_mode:
	mov al, "L"
	jmp error

setup_page_tables:
	lea eax, [rel page_table_l4]
    mov [rel pml4_phys_addr], eax
	mov eax, page_table_l3
	or eax, 0b11 ; present, writable
	mov [page_table_l4], eax
	
	mov eax, page_table_l2
	or eax, 0b11 ; present, writable
	mov [page_table_l3], eax

	mov ecx, 0 ; counter

	mov eax, page_table_l3_mmio
    or eax, 0b11            ; present, writable
    mov [page_table_l4 + 511*8], eax

    mov eax, page_table_l2_mmio
    or eax, 0b11            ; present, writable
    mov [page_table_l3_mmio], eax

.loop:

	mov eax, 0x200000 ; 2MiB
	mul ecx
	or eax, 0b10000011 ; present, writable, huge page
	mov [page_table_l2 + ecx * 8], eax

	inc ecx ; increment counter
	cmp ecx, 512 ; checks if the whole table is mapped
	jne .loop ; if not, continue

	ret

enable_paging:
	; pass page table location to cpu
	mov eax, page_table_l4
	mov cr3, eax

	; enable PAE
	mov eax, cr4
	or eax, 1 << 5
	mov cr4, eax

	; enable long mode
	mov ecx, 0xC0000080
	rdmsr
	or eax, 1 << 8
	wrmsr

	; enable paging
	mov eax, cr0
	or eax, 1 << 31
	mov cr0, eax

	ret

error:
	; print "ERR: X" where X is the error code
	mov dword [0xb8000], 0x4f524f45
	mov dword [0xb8004], 0x4f3a4f52
	mov dword [0xb8008], 0x4f204f20
	mov byte  [0xb800a], al
	hlt


section .data
align 16
idt_descriptor64:
    dw idt_end - idt_base      ; Limit
    dq idt_base                ; Base address

idt_base:
    times 256 dq 0             ; 256 IDT entries
idt_end:

align 4096
page_table_l4:
times 512 dq 0

align 4096
page_table_l3:
times 512 dq 0

align 4096
page_table_l2:
times 512 dq 0

align 4096
global page_table_l3_mmio
page_table_l3_mmio: times 512 dq 0

align 4096
global page_table_l2_mmio
page_table_l2_mmio: times 512 dq 0


tss_struct:
    ; zero out the first 104 bytes (size of TSS excluding IST entries)
    times 104 db 0

    ; IST array starts at offset 36 bytes from the start of the TSS
    ; IST1 (first IST entry) = double_fault_stack_top
    dq double_fault_stack_top

    ; zero fill remaining IST entries (IST2..IST7)
    times (7*8 - 8) db 0


align 16
stack_bottom:
times 4096 * 4 db 0
stack_top:

section .rodata
gdt64:
    dq 0                       ; null descriptor
.code_segment: equ $ - gdt64
    dq 0x00209A0000000000      ; 64-bit code segment descriptor

.pointer:
    dw $ - gdt64 - 1           ; limit (size of GDT - 1)
    dq gdt64                   ; base address