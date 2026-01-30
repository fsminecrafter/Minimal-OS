#pragma once
#include <stdint.h>
#include <stdnoreturn.h>

typedef struct RegisterState {
	uint64_t rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip, rflags;
} RegisterState;

#define PANIC(msg) panic(msg, __FILE__, __LINE__, NULL)
#define PANIC_WITH_REGS(msg, regs) panic(msg, __FILE__, __LINE__, regs)

noreturn void panic(const char* message, const char* file, int line, const RegisterState* regs);
