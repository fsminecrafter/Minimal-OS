global panic_wrapper

extern panic

section .text
panic_wrapper:
	; push general-purpose registers
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push rsp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15

	; push RIP and RFLAGS manually (assume we come from iret-compatible handler)
	mov rax, [rsp+120]  ; RIP pushed automatically by CPU
	push rax
	pushfq

	; allocate space for struct RegisterState
	sub rsp, 19*8
	mov rdi, rsp

	; copy 17 registers into [rsp] (the struct)
	pop qword [rdi+17*8] ; rflags
	pop qword [rdi+16*8] ; rip
	pop qword [rdi+15*8] ; r15
	pop qword [rdi+14*8]
	pop qword [rdi+13*8]
	pop qword [rdi+12*8]
	pop qword [rdi+11*8]
	pop qword [rdi+10*8]
	pop qword [rdi+9*8]
	pop qword [rdi+8*8]
	pop qword [rdi+7*8]
	pop qword [rdi+6*8]
	pop qword [rdi+5*8]
	pop qword [rdi+4*8]
	pop qword [rdi+3*8]
	pop qword [rdi+2*8]
	pop qword [rdi+1*8]
	pop qword [rdi+0*8] ; rax

	; Now [rdi] has RegisterState*
	mov rsi, panic_msg
	mov rdx, panic_file
	mov ecx, panic_line
	call panic

panic_msg: db "Fatal interrupt", 0
panic_file: db "interrupt.asm", 0
panic_line: equ 999

