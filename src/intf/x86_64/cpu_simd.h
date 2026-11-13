// src/intf/x86_64/cpu_simd.h
//
// SIMD state of the CPU: XSAVE / AVX / AVX-512 enabling, the save area the
// interrupt, exception and syscall entry stubs use, and the CPU feature
// mask SYS_SYSINFO reports to programs.
#ifndef CPU_SIMD_H
#define CPU_SIMD_H

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/multiboot2parse.h"

// Read by the entry stubs (simd_save.inc), so they keep these exact names
// and sizes.
//   simd_use_xsave  0: save with fxsave64 (512 bytes), 1: with xsave64
//   simd_xcr0       the XCR0 value every CPU runs with (the xsave mask)
//   simd_save_size  bytes the stubs reserve on the stack for one save:
//                   512, or the CPUID.(0xD,0).EBX size rounded up to 64
extern uint8_t  simd_use_xsave;
extern uint64_t simd_xcr0;
extern uint64_t simd_save_size;

// The 'noavx' boot flag: keep the SSE-only fxsave behaviour even on a CPU
// with AVX (a fallback for hardware trouble).
extern bool kernel_boot_noavx;

// Boot CPU: reads the 'noavx' flag, decides between fxsave and xsave, turns
// XSAVE/AVX on for this CPU and works out the feature mask. Must run in
// kernel_main before the IDT is loaded and interrupts are enabled: the entry
// stubs read the switch above, and changing it while a save is in flight
// would restore an area in the wrong format.
void cpu_simd_init_bsp(multiboot2_info_t* mb_info);

// Application processors: the same CR4/XCR0 setup as the boot CPU, before
// the AP loads the IDT. Uses the decision the boot CPU made.
void cpu_simd_init_ap(uint32_t cpu_id);

// SYS_CPU_* bit mask (x86_64/syscall.h). The AVX-family bits are only set
// when the OS enabled the matching state, so a set bit means "usable".
uint64_t cpu_simd_features(void);

#endif
