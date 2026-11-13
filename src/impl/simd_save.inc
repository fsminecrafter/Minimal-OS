; simd_save.inc - save/restore the interrupted code's x87/SSE/AVX state on the
; kernel stack. Used by every kernel entry stub (exceptions, IRQs, int 0x80).
;
; This is also how a process's SIMD state survives a context switch: nothing
; saves it in context_switch.asm. A switch happens inside a timer IRQ or a
; blocking syscall, i.e. deeper than one of these stubs, so the outgoing
; process's state is already sitting in its own kernel stack, and the
; incoming process restores its own when it unwinds through its stub.
;
; SIMD_SAVE clobbers rax, rcx, rdx and rbp (the caller has already pushed
; them or loaded nothing it needs from them) and leaves rbp = the rsp it had
; on entry; SIMD_RESTORE brings rsp back from rbp. The area is 64-byte
; aligned (xsave requires it) and simd_save_size bytes (cpu_simd.c) large;
; the switch simd_use_xsave picks fxsave64 or xsave64. Both read globals
; that are final before interrupts are first enabled.

extern simd_use_xsave
extern simd_xcr0
extern simd_save_size

%macro SIMD_SAVE 0
    mov rbp, rsp
    sub rsp, [rel simd_save_size]
    and rsp, -64
    cmp byte [rel simd_use_xsave], 0
    jne %%xsave
    fxsave64 [rsp]
    jmp %%done
%%xsave:
    ; The XSAVE header (bytes 512..575) must be zero: xsave only writes
    ; XSTATE_BV into it and xrstor faults on stale reserved bits.
%assign off 512
%rep 8
    mov qword [rsp + off], 0
%assign off off+8
%endrep
    mov rax, [rel simd_xcr0]
    mov rdx, rax
    shr rdx, 32
    xsave64 [rsp]
%%done:
%endmacro

%macro SIMD_RESTORE 0
    cmp byte [rel simd_use_xsave], 0
    jne %%xrstor
    fxrstor64 [rsp]
    jmp %%done
%%xrstor:
    mov rax, [rel simd_xcr0]
    mov rdx, rax
    shr rdx, 32
    xrstor64 [rsp]
%%done:
    mov rsp, rbp
%endmacro
