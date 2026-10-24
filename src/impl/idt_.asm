BITS 64

extern idt_handler_keyboard
extern idt_handler_pit
extern idt_handler_doublefault
extern lapic_timer_irq_handler
extern idt_handler_stray_vector
%assign irq 2
%rep 14
extern idt_handler_pic_irq %+ irq
%assign irq irq+1
%endrep

global idt_load

idt_load:
    lidt [rdi]
    ret

%macro WRAPPED_HANDLER 1
global %1_wrapped

%1_wrapped:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save the interrupted code's SSE/x87 state (C code here may use
    ; SSE) and call with a 16-byte aligned stack, as the ABI requires.
    mov rbp, rsp
    sub rsp, 512
    and rsp, -16
    fxsave64 [rsp]
    call %1
    fxrstor64 [rsp]
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    iretq
%endmacro

WRAPPED_HANDLER idt_handler_keyboard
WRAPPED_HANDLER idt_handler_pit
WRAPPED_HANDLER idt_handler_doublefault
WRAPPED_HANDLER lapic_timer_irq_handler
WRAPPED_HANDLER idt_handler_stray_vector

; IRQ2-15 on the legacy PIC (vectors 0x22-0x2F). They are masked, but the
; PIC still raises spurious IRQ7/IRQ15 and the CPU needs a gate for them.
%assign irq 2
%rep 14
WRAPPED_HANDLER idt_handler_pic_irq %+ irq
%assign irq irq+1
%endrep
