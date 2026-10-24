BITS 64

global smp_par_call_on_stack

; void smp_par_call_on_stack(void (*fn)(uint32_t), uint32_t arg, void* stack_top)
smp_par_call_on_stack:
    push rbp
    mov rbp, rsp
    mov rsp, rdx
    and rsp, -16
    mov rax, rdi
    mov edi, esi
    call rax
    mov rsp, rbp
    pop rbp
    ret
