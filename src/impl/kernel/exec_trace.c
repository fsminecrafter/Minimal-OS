#include "x86_64/exec_trace.h"
#include "serial.h"
#include "string.h"
#include "time.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

// ===========================================
// CONFIG
// ===========================================

#define TRACE_MAX_LINES   10000
#define TRACE_YIELD_EVERY 200
#define INT_STACK_MAX     128

// ===========================================
// TRACE STATE
// ===========================================

static bool g_trace_enabled = true;
static uint32_t g_trace_depth = 0;
static uint32_t g_trace_lines = 0;

static char g_trace_filter[64] = {0};
static bool g_has_filter = false;

// ===========================================
// TRACE CALL STACK
// ===========================================

typedef struct {
    const char* file;
    const char* func;
    int line;
    uint64_t timestamp;
} trace_frame_t;

static trace_frame_t g_call_stack[TRACE_MAX_DEPTH];

// ===========================================
// INTERRUPT OWNERSHIP STACK
// ===========================================

typedef struct {
    const char* file;
    const char* func;
    int line;
    uint64_t time;
    bool state;
} int_frame_t;

static int_frame_t g_int_stack[INT_STACK_MAX];
static int g_int_sp = 0;

static bool g_interrupts_enabled = true;
static const char* g_int_owner = 0;

// ===========================================
// HELPERS
// ===========================================

static const char* basename(const char* path) {
    const char* base = path;
    while (*path) {
        if (*path == '/' || *path == '\\') base = path + 1;
        path++;
    }
    return base;
}

static bool matches_filter(const char* func) {
    return (!g_has_filter) || strstr(func, g_trace_filter);
}

// ===========================================
// CPU STATE CHECK
// ===========================================

static inline bool cpu_interrupts_enabled(void) {
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    return rflags & (1 << 9);
}

// ===========================================
// TRACE OUTPUT
// ===========================================

static void trace_print(const char* fmt, ...) {
    if (!g_trace_enabled) return;

    if (g_trace_lines++ > TRACE_MAX_LINES) return;

    char buf[256];
    int offset = 0;

    int indent = g_trace_depth * 2;
    if (indent > 40) indent = 40;

    for (int i = 0; i < indent; i++)
        buf[offset++] = ' ';

    va_list args;
    va_start(args, fmt);
    offset += vsnprintf(buf + offset, sizeof(buf) - offset, fmt, args);
    va_end(args);

    if (offset >= (int)sizeof(buf)) offset = sizeof(buf) - 1;
    buf[offset] = '\0';

    serial_write_str(buf);

    if ((g_trace_lines % TRACE_YIELD_EVERY) == 0) {
        asm volatile("sti; hlt; cli");
    }
}

// ===========================================
// INTERRUPT AUDIT CORE
// ===========================================

static void int_push(const char* file, const char* func, int line, bool state) {
    if (g_int_sp >= INT_STACK_MAX) {
        serial_write_str("!!! INT STACK OVERFLOW !!!\n");
        return;
    }

    if (g_int_owner && strcmp(g_int_owner, func) != 0) {
        serial_write_str("!!! IRQ OWNER CONFLICT !!!\n");
    }

    g_int_stack[g_int_sp++] = (int_frame_t){
        .file = file,
        .func = func,
        .line = line,
        .time = time_get_uptime_ms(),
        .state = state
    };

    g_int_owner = func;
    g_interrupts_enabled = state;
}

// ===========================================
// REQUIRED MISSING SYMBOLS (FIXED)
// ===========================================

// called by irq wrappers (AHCI / FS / etc)
void trace_irq_restore(const char* file, const char* func, int line) {
    if (g_int_sp <= 0) {
        serial_write_str("!!! IRQ RESTORE WITHOUT CONTEXT !!!\n");
        return;
    }

    g_int_sp--;
    g_interrupts_enabled = (g_int_sp == 0)
        ? true
        : g_int_stack[g_int_sp - 1].state;

    if (g_int_sp == 0)
        g_int_owner = 0;

    serial_write_str("[IRQ RESTORE] ");
    serial_write_str(basename(file));
    serial_write_str(":");
    serial_write_str(func);
    serial_write_str(":");
    serial_write_dec(line);
    serial_write_str("\n");
}

// HARD ASSERT CHECK (used by FS / AHCI)
void trace_assert_irq_consistency(const char* file, const char* func, int line) {
    bool cpu = cpu_interrupts_enabled();

    if (cpu != g_interrupts_enabled) {
        serial_write_str("!!! IRQ CONSISTENCY FAILURE !!!\n");
        serial_write_str(basename(file));
        serial_write_str(":");
        serial_write_str(func);
        serial_write_str("\nCPU != TRACKED STATE\n");
    }
}

// FULL DUMP (debug freeze tool)
void trace_dump_interrupt_audit(void) {
    serial_write_str("\n=== IRQ AUDIT DUMP ===\n");

    serial_write_str("Tracked state: ");
    serial_write_str(g_interrupts_enabled ? "ON" : "OFF");
    serial_write_str("\n");

    serial_write_str("CPU state: ");
    serial_write_str(cpu_interrupts_enabled() ? "ON" : "OFF");
    serial_write_str("\n");

    serial_write_str("Stack depth: ");
    serial_write_dec(g_int_sp);
    serial_write_str("\n");

    for (int i = 0; i < g_int_sp; i++) {
        serial_write_str("#");
        serial_write_dec(i);
        serial_write_str(" ");
        serial_write_str(basename(g_int_stack[i].file));
        serial_write_str(":");
        serial_write_str(g_int_stack[i].func);
        serial_write_str(" -> ");
        serial_write_str(g_int_stack[i].state ? "ON" : "OFF");
        serial_write_str("\n");
    }

    serial_write_str("=====================\n");
}

//Function to dump all registers - useful for debugging IRQ context issues
void trace_dump_registers(void) {
    serial_write_str("\n=== REGISTER DUMP ===\n");

    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs, ds, es, fs, gs, ss;
    uint64_t cr0, cr2, cr3, cr4;

    // --- GPR batch 1 ---
    __asm__ volatile (
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        : "=r"(rax), "=r"(rbx), "=r"(rcx), "=r"(rdx)
    );

    // --- GPR batch 2 ---
    __asm__ volatile (
        "mov %%rsi, %0\n"
        "mov %%rdi, %1\n"
        "mov %%rbp, %2\n"
        "mov %%rsp, %3\n"
        : "=r"(rsi), "=r"(rdi), "=r"(rbp), "=r"(rsp)
    );

    // --- GPR batch 3 ---
    __asm__ volatile (
        "mov %%r8,  %0\n"
        "mov %%r9,  %1\n"
        "mov %%r10, %2\n"
        "mov %%r11, %3\n"
        : "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
    );

    // --- GPR batch 4 ---
    __asm__ volatile (
        "mov %%r12, %0\n"
        "mov %%r13, %1\n"
        "mov %%r14, %2\n"
        "mov %%r15, %3\n"
        : "=r"(r12), "=r"(r13), "=r"(r14), "=r"(r15)
    );

    // --- RIP ---
    __asm__ volatile (
        "lea (%%rip), %0"
        : "=r"(rip)
    );

    // --- RFLAGS ---
    __asm__ volatile (
        "pushfq\n"
        "pop %0"
        : "=r"(rflags)
    );

    // --- Segment registers ---
    __asm__ volatile (
        "mov %%cs, %0\n"
        "mov %%ds, %1\n"
        "mov %%es, %2\n"
        "mov %%fs, %3\n"
        "mov %%gs, %4\n"
        "mov %%ss, %5\n"
        : "=r"(cs), "=r"(ds), "=r"(es),
          "=r"(fs), "=r"(gs), "=r"(ss)
    );

    // --- Control registers ---
    __asm__ volatile (
        "mov %%cr0, %0\n"
        "mov %%cr2, %1\n"
        "mov %%cr3, %2\n"
        "mov %%cr4, %3\n"
        : "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4)
    );

    #define REG(name, val) \
        serial_write_str(name ": "); \
        serial_write_hex(val); \
        serial_write_str("\n");

    REG("RAX", rax); REG("RBX", rbx);
    REG("RCX", rcx); REG("RDX", rdx);
    REG("RSI", rsi); REG("RDI", rdi);
    REG("RBP", rbp); REG("RSP", rsp);

    REG("R8 ", r8); REG("R9 ", r9);
    REG("R10", r10); REG("R11", r11);
    REG("R12", r12); REG("R13", r13);
    REG("R14", r14); REG("R15", r15);

    REG("RIP", rip);
    REG("RFLAGS", rflags);

    REG("CS", cs); REG("DS", ds);
    REG("ES", es); REG("FS", fs);
    REG("GS", gs); REG("SS", ss);

    REG("CR0", cr0);
    REG("CR2", cr2);
    REG("CR3", cr3);
    REG("CR4", cr4);

    serial_write_str("=====================\n");
}

// ===========================================
// SAFE WRAPPERS
// ===========================================

void trace_cli(const char* file, const char* func, int line) {
    int_push(file, func, line, false);
    asm volatile("cli");
}

void trace_sti(const char* file, const char* func, int line) {
    trace_irq_restore(file, func, line);
    asm volatile("sti");
}

// ===========================================
// TRACE CORE
// ===========================================

void trace_enter(const char* file, const char* func, int line) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    trace_print("→ %s:%s():%d\n", basename(file), func, line);
    g_trace_depth++;
}

void trace_exit(const char* file, const char* func, int line) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    if (g_trace_depth) g_trace_depth--;
    trace_print("← %s()\n", func);
}

void trace_msg(const char* file, const char* func, int line, const char* msg) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    trace_print("• %s\n", msg);
}

// ===========================================
// TRACE CONTROL
// ===========================================

void trace_init(void) {
    g_trace_enabled = true;
    g_trace_depth = 0;
    g_trace_lines = 0;
    g_int_sp = 0;
    g_int_owner = 0;

    serial_write_str("\n=== TRACE INIT ===\n");
}

void trace_enable(bool enabled) {
    if (!enabled) trace_dump_interrupt_audit();
    g_trace_enabled = enabled;
}

// ===========================================
// COMMANDS
// ===========================================

#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "graphics.h"

void cmd_trace_enable(int argc, const char** argv) {
    trace_enable(true);
    graphics_write_textr("Tracing enabled\n");
}

void cmd_trace_disable(int argc, const char** argv) {
    trace_enable(false);
    graphics_write_textr("Tracing disabled\n");
}

void cmd_trace_stack(int argc, const char** argv) {
    trace_dump_interrupt_audit();
}

void register_trace_commands(void) {
    command_register("trace_on", cmd_trace_enable);
    command_register("trace_off", cmd_trace_disable);
    command_register("trace_stack", cmd_trace_stack);
}

REGISTER_COMMAND(register_trace_commands);

void trace_loop(const char* file, const char* func, int line, uint32_t current, uint32_t total) { 
    if (!g_trace_enabled || !matches_filter(func)) return;
    if (total > 0) { 
        uint32_t percent = (current * 100) / total;
        trace_print("• Loop %u/%u (%u%%)\n", current, total, percent); 
    } else { 
        trace_print("• Loop %u/%u\n", current, total); 
    } 
}

void trace_value(const char* file, const char* func, int line,
                const char* name, uint64_t value) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    trace_print("• %s=%llu\n", name, value);
}

void trace_ptr(const char* file, const char* func, int line,
              const char* name, void* ptr) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    trace_print("• %s=0x%llx\n", name, (uint64_t)ptr);
}

void trace_bool(const char* file, const char* func, int line,
               const char* name, bool value) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    trace_print("• %s=%s\n", name, value ? "true" : "false");
}

void trace_hex(const char* file, const char* func, int line,
              const char* name, uint64_t value) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    trace_print("• %s=0x%llx\n", name, value);
}

void trace_line(const char* file, const char* func, int line) {
    if (!g_trace_enabled || !matches_filter(func)) return;
    trace_print("• %s:%d\n", basename(file), line);
}