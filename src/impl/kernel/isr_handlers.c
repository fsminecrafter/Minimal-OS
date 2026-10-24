#include <stdint.h>
#include "serial.h"
#include "panic.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/isr_crash.h"

static user_crash_info_t g_last_crash;

const user_crash_info_t* isr_last_user_crash(void) {
    return &g_last_crash;
}

static const char* exception_name(uint64_t vector) {
    switch (vector) {
        case 0:  return "Divide Error";
        case 1:  return "Debug";
        case 2:  return "NMI";
        case 3:  return "Breakpoint";
        case 4:  return "Overflow";
        case 5:  return "BOUND Range Exceeded";
        case 6:  return "Invalid Opcode";
        case 7:  return "Device Not Available";
        case 8:  return "Double Fault";
        case 9:  return "Coprocessor Segment Overrun";
        case 10: return "Invalid TSS";
        case 11: return "Segment Not Present";
        case 12: return "Stack-Segment Fault";
        case 13: return "General Protection Fault";
        case 14: return "Page Fault";
        case 16: return "x87 FPU Error";
        case 17: return "Alignment Check";
        case 18: return "Machine Check";
        case 19: return "SIMD FP Exception";
        case 20: return "Virtualization Exception";
        case 21: return "Control Protection Exception";
        case 28: return "Hypervisor Injection Exception";
        case 29: return "VMM Communication Exception";
        case 30: return "Security Exception";
        default: return "Reserved/Unknown Exception";
    }
}

/*
 * Common landing point for every CPU exception vector 0-31 except 8
 * (double fault keeps its own dedicated IST1 handler - see idt_init()).
 * Previously these all hit a zeroed/absent IDT gate, which on real
 * hardware and in QEMU means an immediate triple fault / silent reset
 * with zero diagnostic output. Now they produce a readable message;
 * exceptions from CPL3 terminate only the faulting user process.
 */
void isr_generic_handler(uint64_t vector, uint64_t error_code,
                         uint64_t saved_rip, uint64_t saved_cs) {
    // panic() stops the other cores with an NMI: just halt here.
    if (vector == 2 && panic_in_progress()) {
        // isr_common_stub's frame pointer slot holds the interrupted rbp.
        uint64_t* stub_frame = (uint64_t*)__builtin_frame_address(0);
        panic_record_cpu(saved_rip, saved_cs, *(uint64_t*)stub_frame[0]);
        for (;;) asm volatile("cli; hlt");
    }
    if (vector == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        serial_write_str("\n!!! PAGE FAULT !!!\n  CR2 (faulting addr): 0x");
        serial_write_hex(cr2);
        serial_write_str("\n  Error code: 0x");
        serial_write_hex(error_code);
        serial_write_str(" (");
        serial_write_str((error_code & 1) ? "present" : "not-present");
        serial_write_str(", ");
        serial_write_str((error_code & 2) ? "write" : "read");
        serial_write_str((error_code & 4) ? ", user-mode" : ", kernel-mode");
        serial_write_str(")\n");
    } else {
        serial_write_str("\n!!! UNHANDLED CPU EXCEPTION !!!\n  Vector: ");
        serial_write_dec(vector);
        serial_write_str(" (");
        serial_write_str(exception_name(vector));
        serial_write_str(")\n  Error code: 0x");
        serial_write_hex(error_code);
        serial_write_str("\n");
    }

    serial_write_str("  Saved RIP: 0x");
    serial_write_hex(saved_rip);
    serial_write_str("\n");

    if ((saved_cs & 3) == 3 && current_process &&
        current_process->privilege == PROC_PRIVILEGE_USER &&
        vector != 2 && vector != 18) {
        // Kept for the terminal's crash message: machines without a
        // serial port only see the screen.
        uint64_t cr2 = 0;
        if (vector == 14) asm volatile("mov %%cr2, %0" : "=r"(cr2));
        g_last_crash.pid = current_process->pid;
        g_last_crash.vector = (uint8_t)vector;
        g_last_crash.error_code = error_code;
        g_last_crash.rip = saved_rip;
        g_last_crash.fault_address = cr2;
        g_last_crash.image_base = current_process->user_image_address;
        g_last_crash.image_size = current_process->user_image_size;
        for (uint32_t i = 0; i < sizeof(g_last_crash.name) - 1; i++) {
            g_last_crash.name[i] = current_process->name[i];
            if (!current_process->name[i]) break;
        }
        g_last_crash.name[sizeof(g_last_crash.name) - 1] = 0;
        serial_write_str("  Image base: 0x");
        serial_write_hex(current_process->user_image_address);
        serial_write_str("\n");
        serial_write_str("  Terminating faulting user process: ");
        serial_write_str(current_process->name);
        serial_write_str("\n");
        process_exit_with_status(0xFF, true, (uint8_t)vector);
    }

    // Kernel-mode exception: put the details in the panic message, since
    // the screen is often the only thing the user can see.
    static char message[160];
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    const char* parts[] = { "CPU exception ", exception_name(vector) };
    for (int p = 0; p < 2; p++)
        for (const char* c = parts[p]; *c && n < 100; c++) message[n++] = *c;
    const char* labels[] = { " vec=0x", " err=0x", " rip=0x" };
    uint64_t values[] = { vector, error_code, saved_rip };
    for (int p = 0; p < 3; p++) {
        for (const char* c = labels[p]; *c; c++) message[n++] = *c;
        int digits = p == 2 ? 16 : 4;
        for (int i = digits - 1; i >= 0; i--) message[n++] = hex[(values[p] >> (4 * i)) & 0xF];
    }
    if (vector == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        for (const char* c = " cr2=0x"; *c; c++) message[n++] = *c;
        for (int i = 15; i >= 0; i--) message[n++] = hex[(cr2 >> (4 * i)) & 0xF];
    }
    message[n] = 0;
    PANIC(message);
}