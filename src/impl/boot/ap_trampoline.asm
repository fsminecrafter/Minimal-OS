; Real-mode AP entry stub. APs always start execution here in 16-bit
; real mode with CS:IP = (vector<<8):0 regardless of BSP mode - this
; redoes the real->protected->long mode transition from main.asm, but
; reuses the BSP's already-built PML4 (shared across all cores, same
; as every process_t.pml4 already does) instead of building its own.
;
; Loaded by smp.c at the fixed physical address AP_TRAMPOLINE_ADDR
; (0x8000). The last 28 bytes of this file are a "mailbox" the BSP
; pokes with per-AP values before sending SIPI - see ap_mailbox_t in
; smp.c, which must match this layout exactly.

BITS 16
ORG 0x8000

ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00        ; scratch stack, only used briefly

    lgdt [gdt_ptr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode_entry

BITS 32
protected_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, cr4
    or eax, (1 << 5)       ; PAE
    mov cr4, eax

    mov eax, [mailbox_pml4]
    mov cr3, eax

    mov ecx, 0xC0000080    ; EFER
    rdmsr
    or eax, (1 << 8)       ; LME
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)      ; PG - activates long mode now LME+PG both set
    mov cr0, eax

    jmp 0x18:long_mode_entry

BITS 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [mailbox_stack_top]
    mov edi, [mailbox_cpu_id]   ; System V ABI: 1st arg
    mov rax, [mailbox_entry]
    jmp rax                     ; jmp not call - ap_entry_c() never returns,
                                 ; same reasoning as proc_trampoline() in proc.c

ALIGN 8
gdt_start:
    dq 0x0000000000000000              ; null
    dq 0x00CF9A000000FFFF              ; 0x08: 32-bit code, flat
    dq 0x00CF92000000FFFF              ; 0x10: 32-bit data, flat
    dq 0x00209A0000000000              ; 0x18: 64-bit code (L bit) - same magic as gdt64 in main.asm
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

ALIGN 8
mailbox_pml4:      dq 0
mailbox_stack_top: dq 0
mailbox_entry:      dq 0
mailbox_cpu_id:      dd 0

ap_trampoline_end: