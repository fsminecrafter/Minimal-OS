; src/impl/x86_64/proc/context_switch.asm
section .text
global context_switch

; RDI = old process_t*, RSI = new process_t*

context_switch:
    ; ---- SAVE OLD CONTEXT ----

    ; regs[] starts at offset 0xA8 in process_t
    ; Save RBX, RBP, R12–R15 into regs[0..5]
    mov [rdi + 0xA8 + 0*8], rbx    ; regs[0]
    mov [rdi + 0xA8 + 1*8], rbp    ; regs[1]
    mov [rdi + 0xA8 + 2*8], r12    ; regs[2]
    mov [rdi + 0xA8 + 3*8], r13    ; regs[3]
    mov [rdi + 0xA8 + 4*8], r14    ; regs[4]
    mov [rdi + 0xA8 + 5*8], r15    ; regs[5]

    ; Save RSP → regs[6]
    mov [rdi + 0xA8 + 6*8], rsp    ; regs[6]

    ; Save RFLAGS → regs[8]
    pushfq
    pop  qword [rdi + 0xA8 + 8*8]  ; regs[8]

    ; Save return RIP → regs[7]
    lea  rax, [rel return_point]
    mov  [rdi + 0xA8 + 7*8], rax   ; regs[7]

    ; Save PML4 (just in case) in pid field? (optional)

    ; ---- SWITCH PAGE TABLE ----

    ; pml4 pointer lives at offset 0x88 in process_t
    mov  rax, [rsi + 0x88]         ; new->pml4
    mov  cr3, rax

    ; ---- RESTORE NEW CONTEXT ----

    ; Restore RBX, RBP, R12–R15 from regs[0..5]
    mov  rbx, [rsi + 0xA8 + 0*8]
    mov  rbp, [rsi + 0xA8 + 1*8]
    mov  r12, [rsi + 0xA8 + 2*8]
    mov  r13, [rsi + 0xA8 + 3*8]
    mov  r14, [rsi + 0xA8 + 4*8]
    mov  r15, [rsi + 0xA8 + 5*8]

    ; Restore RSP
    mov  rsp, [rsi + 0xA8 + 6*8]   ; regs[6]

    ; Restore RFLAGS
    push qword [rsi + 0xA8 + 8*8]   ; regs[8]
    popfq

    ; Jump to new RIP
    jmp  qword [rsi + 0xA8 + 7*8]   ; regs[7]

return_point:
    ret
