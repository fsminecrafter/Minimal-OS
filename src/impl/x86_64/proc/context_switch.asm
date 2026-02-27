; src/impl/x86_64/proc/context_switch.asm
section .text
global context_switch

; RDI = old process_t*, RSI = new process_t*

; CORRECT OFFSETS:
; pml4 at 0xD8, kernel_stack at 0xE0, regs at 0x90

context_switch:
    ; Save old context to regs[] at offset 0x90
    mov [rdi + 0x0000000000000090 + 0*8], rbx    ; regs[0]
    mov [rdi + 0x0000000000000090 + 1*8], rbp    ; regs[1]
    mov [rdi + 0x0000000000000090 + 2*8], r12    ; regs[2]
    mov [rdi + 0x0000000000000090 + 3*8], r13    ; regs[3]
    mov [rdi + 0x0000000000000090 + 4*8], r14    ; regs[4]
    mov [rdi + 0x0000000000000090 + 5*8], r15    ; regs[5]
    mov [rdi + 0x0000000000000090 + 6*8], rsp    ; regs[6]
    
    pushfq
    pop qword [rdi + 0x0000000000000090 + 8*8]   ; regs[8]
    
    lea rax, [rel return_point]
    mov [rdi + 0x0000000000000090 + 7*8], rax    ; regs[7]
    
    ; Load PML4 from offset 0xD8 (NOT 0x88!)
    mov rax, [rsi + 0x00000000000000D8]
    mov cr3, rax
    
    ; Restore new context from regs[] at offset 0x90
    mov rbx, [rsi + 0x0000000000000090 + 0*8]
    mov rbp, [rsi + 0x0000000000000090 + 1*8]
    mov r12, [rsi + 0x0000000000000090 + 2*8]
    mov r13, [rsi + 0x0000000000000090 + 3*8]
    mov r14, [rsi + 0x0000000000000090 + 4*8]
    mov r15, [rsi + 0x0000000000000090 + 5*8]
    mov rsp, [rsi + 0x0000000000000090 + 6*8]
    
    push qword [rsi + 0x0000000000000090 + 8*8]
    popfq
    
    jmp qword [rsi + 0x0000000000000090 + 7*8]

return_point:
    ret