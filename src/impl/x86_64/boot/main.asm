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
    lea eax, [page_table_l4]
    mov [pml4_phys_addr], eax

    mov eax, page_table_l3
    or eax, 0b11
    mov [page_table_l4], eax

    mov eax, page_table_l2
    or eax, 0b11
    mov [page_table_l3], eax

    ; MMIO high mapping
    mov eax, page_table_l3_mmio
    or eax, 0b11
    mov [page_table_l4 + 511*8], eax

    mov eax, page_table_l2_mmio
    or eax, 0b11
    mov [page_table_l3_mmio], eax

    xor ecx, ecx

.loop:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011   ; present + writable + huge
    mov [page_table_l2 + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .loop

    ret


enable_paging:
    ; load PML4
    mov eax, page_table_l4
    mov cr3, eax

    ; enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; enable long mode + NX (future-proof)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)   ; LME + NXE
    wrmsr

    ; enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ret


error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt


section .data
align 16
idt_descriptor64:
    dw idt_end - idt_base
    dq idt_base

idt_base:
    times 256 dq 0
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
page_table_l3_mmio:
    times 512 dq 0

align 4096
page_table_l2_mmio:
    times 512 dq 0


tss_struct:
    times 104 db 0
    dq double_fault_stack_top
    times (7*8 - 8) db 0


align 16
stack_bottom:
    times 4096 * 4 db 0
stack_top:


section .rodata
gdt64:
    dq 0
.code_segment: equ $ - gdt64
    dq 0x00209A0000000000

.pointer:
    dw $ - gdt64 - 1
    dq gdt64