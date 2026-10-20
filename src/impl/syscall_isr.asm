BITS 64

extern syscall_dispatch
global isr_syscall_wrapped

; int 0x80 entry point (no CPU error code for this vector).
; Push order below, top-of-stack first = rax, matches syscall_regs_t
; in x86_64/syscall.h field-for-field.
isr_syscall_wrapped:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rsp        ; syscall_regs_t*
    ; User code expects its SSE registers to survive the int 0x80 (the
    ; SDK's inline asm does not list them as clobbered), so save them.
    mov rbp, rsp
    sub rsp, 512
    and rsp, -16
    fxsave64 [rsp]
    call syscall_dispatch
    fxrstor64 [rsp]
    mov rsp, rbp

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    iretq