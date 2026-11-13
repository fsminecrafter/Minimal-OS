BITS 64

extern isr_generic_handler

global isr_spurious
global isr_common_stub

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0          ; dummy error code (CPU doesn't push one for this vector)
    push qword %1         ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1          ; vector number (CPU already pushed a real error code below this)
    jmp isr_common_stub
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; On entry: [rsp+0]=vector, [rsp+8]=error_code, [rsp+16]=saved RIP.
; Every register the C handler may change is saved: a page fault that the
; handler resolves (uvm_handle_fault) returns to the faulting instruction.
isr_common_stub:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push rbp

    mov rdi, [rsp + 80]        ; arg1 = vector
    mov rsi, [rsp + 88]        ; arg2 = error_code
    mov rdx, [rsp + 96]        ; arg3 = saved RIP
    mov rcx, [rsp + 104]       ; arg4 = saved CS (privilege level)

    mov rbp, rsp
    sub rsp, 512
    and rsp, -16
    fxsave64 [rsp]
    cld
    call isr_generic_handler
    fxrstor64 [rsp]
    mov rsp, rbp

    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

    add rsp, 16             ; drop vector + error_code
    iretq

; Spurious LAPIC interrupt (vector 0xFF). Per the SDM this must NOT be
; EOI'd - the APIC didn't actually deliver a real interrupt, so there's
; nothing to acknowledge. Just return.
isr_spurious:
    iretq