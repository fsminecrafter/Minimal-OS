#include <stdint.h>
#include <stddef.h>
#include "x86_64/proc.h"
#include "serial.h"

// Standalone utility to verify process_t struct offsets
// Call this once from main to verify your context_switch.asm is correct

void verify_process_offsets(void) {
    serial_write_str("\n");
    serial_write_str("========================================\n");
    serial_write_str("  PROCESS_T OFFSET VERIFICATION\n");
    serial_write_str("========================================\n\n");
    
    serial_write_str("sizeof(process_t) = ");
    serial_write_dec(sizeof(process_t));
    serial_write_str(" bytes\n\n");
    
    // Calculate offsets
    uint64_t regs_offset = offsetof(process_t, regs);
    uint64_t pml4_offset = offsetof(process_t, pml4);
    
    serial_write_str("Critical Offsets:\n");
    serial_write_str("  regs[9]:  0x");
    serial_write_hex(regs_offset);
    serial_write_str("\n");
    serial_write_str("  pml4:     0x");
    serial_write_hex(pml4_offset);
    serial_write_str("\n\n");
    
    serial_write_str("Your context_switch.asm should use:\n");
    serial_write_str("  mov [rdi + 0x");
    serial_write_hex(regs_offset);
    serial_write_str(" + N*8], reg\n");
    serial_write_str("  mov rax, [rsi + 0x");
    serial_write_hex(pml4_offset);
    serial_write_str("]\n\n");
    
    serial_write_str("========================================\n\n");
}