#include "panic.h"
#include "print.h"

#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25

static void print_centered(const char* text, int row) {
	int len = 0;
	while (text[len]) len++;

	int col = (SCREEN_WIDTH - len) / 2;
	print_str_at(text, col, row);
}

static void print_registers(const RegisterState* regs, int start_row) {
	if (!regs) return;

	char buffer[64];

	print_str_at("Register dump:", 0, start_row++);

#define PRINT_REG(name, val) { \
	print_str_at(#name ": 0x", 0, start_row); \
	print_hex64_at(val, 12, start_row++); \
}

	PRINT_REG(RAX, regs->rax)
	PRINT_REG(RBX, regs->rbx)
	PRINT_REG(RCX, regs->rcx)
	PRINT_REG(RDX, regs->rdx)
	PRINT_REG(RSI, regs->rsi)
	PRINT_REG(RDI, regs->rdi)
	PRINT_REG(RBP, regs->rbp)
	PRINT_REG(RSP, regs->rsp)
	PRINT_REG(R8,  regs->r8)
	PRINT_REG(R9,  regs->r9)
	PRINT_REG(R10, regs->r10)
	PRINT_REG(R11, regs->r11)
	PRINT_REG(R12, regs->r12)
	PRINT_REG(R13, regs->r13)
	PRINT_REG(R14, regs->r14)
	PRINT_REG(R15, regs->r15)
	PRINT_REG(RIP, regs->rip)
	PRINT_REG(RFLAGS, regs->rflags)

#undef PRINT_REG
}

noreturn void panic(const char* message, const char* file, int line, const RegisterState* regs) {
	asm volatile("cli");

	// Blue background, white text
	print_clear_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLUE);

	int row = 3;
	print_centered("!!! KERNEL PANIC !!!", row++);
	print_centered(message, row++);
	print_centered(file, row++);
	
	char line_str[16];
	print_centered("Line: ", row);
	print_int_at(line, 42, row++);
	row++;

	print_registers(regs, row);

	while (1) {
		asm volatile("hlt");
	}
}
