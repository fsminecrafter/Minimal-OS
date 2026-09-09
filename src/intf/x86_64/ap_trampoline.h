#ifndef AP_TRAMPOLINE_H
#define AP_TRAMPOLINE_H

#include <stdint.h>

// Physical load address for the trampoline - page-aligned, below 1MB.
// STARTUP IPI vector = addr >> 12 = 0x08. Conventionally free in both
// real hardware and QEMU; move it if anything else claims this page.
#define AP_TRAMPOLINE_ADDR 0x8000

extern const uint8_t _binary_ap_trampoline_bin_start[];
extern const uint8_t _binary_ap_trampoline_bin_end[];

#endif // AP_TRAMPOLINE_H