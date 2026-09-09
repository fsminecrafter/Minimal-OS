#include <stdint.h>
#include "serial.h"
#include "panic.h"

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
 * with zero diagnostic output. Now they at least produce a readable
 * message before panicking.
 */
void isr_generic_handler(uint64_t vector, uint64_t error_code) {
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

    PANIC("Unhandled CPU exception");
}