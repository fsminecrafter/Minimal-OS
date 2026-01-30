extern idt_handler_keyboard
extern idt_handler_pit
extern idt_handler_doublefault
global idt_load
global lidt

idt_load:
	lidt [rdi]
	ret

lidt:
    mov rax, [rdi]          ; load limit
    mov rdx, [rdi + 8]      ; load base
    lidt [rdi]              ; rdi points to IDTPointer (limit + base)
    ret

%macro WRAPPED_HANDLER 1
	global %1_wrapped
	
	%1_wrapped:
		; save general-purpose registers
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

		call %1

		; restore general-purpose registers
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