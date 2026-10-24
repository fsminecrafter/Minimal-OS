#include "panic.h"
#include "print.h"
#include "x86_64/exec_trace.h"
#include "graphics.h"
#include "x86_64/lapic.h"
#include "serial.h"
#include "x86_64/smp.h"
#include "x86_64/proc.h"

extern char font8x8_basic[128][8];

/*
 * The VGA text buffer print_* writes to is not shown on a framebuffer
 * (UEFI/GOP) machine or while a program owns the screen, so a panic there
 * looked like a silent freeze. This draws the same lines straight into
 * the framebuffer, without the graphics lock (it may be held by the code
 * that panicked).
 */
#define PANIC_FB_SCALE 2
#define PANIC_FB_LINE  (10 * PANIC_FB_SCALE)
static int g_panic_fb_line;
static volatile uint32_t g_panic_active;
static const gpu_device_t* g_panic_screen;

static void panic_fb_fill(uint32_t color) {
	if (!g_panic_screen || !g_panic_screen->fb || g_panic_screen->bpp != 4) return;
	for (uint32_t y = 0; y < g_panic_screen->height; y++) {
		volatile uint32_t* row = (volatile uint32_t*)((volatile uint8_t*)g_panic_screen->fb + (uint64_t)y * g_panic_screen->pitch);
		for (uint32_t x = 0; x < g_panic_screen->width; x++) row[x] = color;
	}
}

static void panic_fb_glyph(uint32_t x0, uint32_t y0, uint8_t c) {
	if (c >= 128) c = '?';
	for (uint32_t gy = 0; gy < 8; gy++) {
		uint8_t bits = (uint8_t)font8x8_basic[c][gy];
		for (uint32_t sy = 0; sy < PANIC_FB_SCALE; sy++) {
			volatile uint32_t* row = (volatile uint32_t*)((volatile uint8_t*)g_panic_screen->fb +
				(uint64_t)(y0 + gy * PANIC_FB_SCALE + sy) * g_panic_screen->pitch);
			for (uint32_t gx = 0; gx < 8; gx++) {
				uint32_t color = (bits & (1u << gx)) ? 0xFFFFFFFFu : 0xFF1A3A8Au;
				for (uint32_t sx = 0; sx < PANIC_FB_SCALE; sx++)
					row[x0 + gx * PANIC_FB_SCALE + sx] = color;
			}
		}
	}
}

// One line of text, wrapped at the screen edge.
static void panic_fb_text(const char* text) {
	if (!text) text = "";
	serial_write_str(text);
	serial_write_str("\n");
	if (!g_panic_screen || !g_panic_screen->fb || g_panic_screen->bpp != 4) return;
	const uint32_t glyph = 8 * PANIC_FB_SCALE;
	do {
		uint32_t y0 = 16 + (uint32_t)g_panic_fb_line++ * PANIC_FB_LINE;
		if (y0 + glyph > g_panic_screen->height) return;
		for (uint32_t x0 = 16; *text && x0 + glyph <= g_panic_screen->width; x0 += glyph, text++)
			panic_fb_glyph(x0, y0, (uint8_t)*text);
	} while (*text);
}

static void panic_hex(char* out, uint64_t v);

// What each core was doing when panic() stopped it, filled in by the
// NMI handler (panic_record_cpu) and by the panicking core itself.
#define PANIC_TRACE_DEPTH 6
typedef struct {
	volatile uint32_t recorded;
	uint64_t rip, cs;
	uint64_t trace[PANIC_TRACE_DEPTH];
	volatile uint32_t trace_count;
} panic_cpu_state_t;
static panic_cpu_state_t g_panic_cpu[MAX_CPUS];

static int panic_frame_ok(uint64_t rbp) {
	return rbp >= 0x1000 && rbp < 0x0000800000000000ULL && (rbp & 7) == 0;
}

// Walks the frame-pointer chain (the kernel is built without
// -fomit-frame-pointer). Each step is stored before the next is read, so
// a bad pointer that faults still leaves the frames found so far.
static void panic_walk(panic_cpu_state_t* st, uint64_t rbp) {
	for (int i = 0; i < PANIC_TRACE_DEPTH && panic_frame_ok(rbp); i++) {
		uint64_t* frame = (uint64_t*)rbp;
		uint64_t ret = frame[1];
		if (ret < 0x100000 || ret >= 0x100000000ULL) break;   // not kernel code
		st->trace[i] = ret;
		st->trace_count = (uint32_t)(i + 1);
		if (frame[0] <= rbp) break;
		rbp = frame[0];
	}
}

void panic_record_cpu(uint64_t rip, uint64_t cs, uint64_t rbp) {
	uint32_t cpu = smp_current_cpu_id();
	if (cpu >= MAX_CPUS) return;
	panic_cpu_state_t* st = &g_panic_cpu[cpu];
	st->rip = rip;
	st->cs = cs;
	st->trace_count = 0;
	__atomic_store_n(&st->recorded, 1, __ATOMIC_RELEASE);
	if ((cs & 3) == 0) panic_walk(st, rbp);
}

static void panic_append(char* buf, int* n, const char* text) {
	while (*text && *n < 94) buf[(*n)++] = *text++;
	buf[*n] = 0;
}

// Hex without leading zeros (kernel addresses are short).
static void panic_append_hex(char* buf, int* n, uint64_t v) {
	char h[19];
	panic_hex(h, v);
	const char* digits = h + 2;
	while (*digits == '0' && digits[1]) digits++;
	panic_append(buf, n, "0x");
	panic_append(buf, n, digits);
}

static void panic_append_dec(char* buf, int* n, uint64_t v) {
	char d[21];
	int k = 0;
	do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	char out[21];
	int o = 0;
	while (k) out[o++] = d[--k];
	out[o] = 0;
	panic_append(buf, n, out);
}

// One line per core: where it was and what it was running, then a short
// call trace. Look the addresses up with
//   addr2line -f -e dist/x86_64/kernel.bin <address>
static void panic_report_cpus(uint32_t self) {
	uint32_t online = smp_online_cpu_count();
	if (online > MAX_CPUS) online = MAX_CPUS;
	// Give the other cores a moment to take the NMI.
	for (uint32_t spin = 0; spin < 20000000u; spin++) {
		uint32_t done = 0;
		for (uint32_t c = 0; c < online; c++) done += g_panic_cpu[c].recorded;
		if (done >= online) break;
		asm volatile("pause");
	}
	char buf[96];
	panic_fb_text("");
	for (uint32_t c = 0; c < online; c++) {
		panic_cpu_state_t* st = &g_panic_cpu[c];
		int n = 0;
		buf[0] = 0;
		panic_append(buf, &n, "cpu");
		panic_append_dec(buf, &n, c);
		panic_append(buf, &n, c == self ? "* " : "  ");
		if (!st->recorded) {
			panic_append(buf, &n, "did not answer (stuck with interrupts off?)");
			panic_fb_text(buf);
			continue;
		}
		panic_append(buf, &n, (st->cs & 3) ? "user   rip=" : "kernel rip=");
		panic_append_hex(buf, &n, st->rip);
		process_t* proc = smp_current_process_for_cpu(c);
		if (proc) {
			panic_append(buf, &n, " pid ");
			panic_append_dec(buf, &n, proc->pid);
			panic_append(buf, &n, " ");
			char name[24];
			int k = 0;
			const char* nm = proc->name;
			for (const char* q = nm; *q; q++) if (*q == '/') nm = q + 1;
			while (nm[k] && k < 23) { name[k] = nm[k]; k++; }
			name[k] = 0;
			panic_append(buf, &n, name);
		}
		panic_fb_text(buf);
		if (st->trace_count) {
			n = 0;
			buf[0] = 0;
			panic_append(buf, &n, "      calls:");
			for (uint32_t i = 0; i < st->trace_count; i++) {
				panic_append(buf, &n, " ");
				panic_append_hex(buf, &n, st->trace[i]);
			}
			panic_fb_text(buf);
		}
	}
}

int panic_in_progress(void) {
	return g_panic_active != 0;
}

static void panic_hex(char* out, uint64_t v) {
	static const char digits[] = "0123456789ABCDEF";
	out[0] = '0'; out[1] = 'x';
	for (int i = 0; i < 16; i++) out[2 + i] = digits[(v >> (60 - 4 * i)) & 0xF];
	out[18] = 0;
}

static void panic_fb_report(const char* message, const char* file, int line, const RegisterState* regs) {
	char buf[96];
	g_panic_screen = graphics_panic_screen();
	panic_fb_fill(0xFF1A3A8Au);
	panic_fb_text("KERNEL PANIC - the system has stopped.");
	panic_fb_text("");
	panic_fb_text(message ? message : "(no message)");
	int n = 0;
	const char* f = file ? file : "?";
	while (*f && n < 70) buf[n++] = *f++;
	buf[n++] = ':';
	char digits[12];
	int d = 0;
	unsigned int l = line < 0 ? 0u : (unsigned int)line;
	do { digits[d++] = (char)('0' + l % 10); l /= 10; } while (l);
	while (d) buf[n++] = digits[--d];
	buf[n] = 0;
	panic_fb_text(buf);
	if (regs) {
		static const char* names[] = { "RIP", "RSP", "RBP", "RAX", "RDI", "RSI" };
		uint64_t vals[] = { regs->rip, regs->rsp, regs->rbp, regs->rax, regs->rdi, regs->rsi };
		for (int i = 0; i < 6; i++) {
			int k = 0;
			for (const char* c = names[i]; *c; c++) buf[k++] = *c;
			buf[k++] = ' ';
			panic_hex(buf + k, vals[i]);
			panic_fb_text(buf);
		}
	}
}


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
	cli();

	// Only the first core to panic reports; the others (stopped by the
	// NMI below, or panicking at the same time) just halt.
	if (__sync_lock_test_and_set(&g_panic_active, 1)) {
		for (;;) asm volatile("cli; hlt");
	}
	lapic_send_nmi_all_but_self();
	panic_fb_report(message, file, line, regs);
	uint32_t self = smp_current_cpu_id();
	panic_record_cpu(regs ? regs->rip : (uint64_t)__builtin_return_address(0), 0,
	                 *(uint64_t*)__builtin_frame_address(0));
	panic_report_cpus(self);
	panic_fb_text("");
	panic_fb_text("Take a photo of this screen, then restart.");

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
