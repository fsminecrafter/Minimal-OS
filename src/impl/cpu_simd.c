// src/impl/cpu_simd.c
//
// XSAVE/AVX bring-up and CPU feature reporting. The kernel itself is
// compiled without AVX: only user programs use the wide registers, and the
// entry stubs (simd_save.inc) save and restore them around every kernel
// entry.
#include "x86_64/cpu_simd.h"
#include "x86_64/syscall.h"
#include "serial.h"

uint8_t  simd_use_xsave = 0;
uint64_t simd_xcr0 = 0;
uint64_t simd_save_size = 512;
bool kernel_boot_noavx = false;

static uint64_t g_cpu_features;

#define CR4_OSXSAVE (1ull << 18)

#define XCR0_X87      (1ull << 0)
#define XCR0_SSE      (1ull << 1)
#define XCR0_AVX      (1ull << 2)
#define XCR0_AVX512   ((1ull << 5) | (1ull << 6) | (1ull << 7)) // opmask, ZMM_Hi256, Hi16_ZMM

// The largest save area the stubs may reserve. AVX-512 needs 2688 bytes;
// anything beyond this means a component we did not ask for crept in.
#define SIMD_MAX_SAVE_SIZE 4096u

static void cpuid(uint32_t leaf, uint32_t sub,
                  uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static uint32_t cpuid_max_leaf(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    return a;
}

static void set_osxsave(bool on) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (on) cr4 |= CR4_OSXSAVE; else cr4 &= ~CR4_OSXSAVE;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

static void write_xcr0(uint64_t value) {
    __asm__ volatile("xsetbv" :: "c"(0), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)) : "memory");
}

// The XCR0 bits this CPU supports (CPUID.(0xD,0).EDX:EAX), or 0 without XSAVE+AVX.
static uint64_t supported_xcr0(void) {
    uint32_t a, b, c, d;
    if (cpuid_max_leaf() < 0xD) return 0;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 26)) || !(c & (1u << 28))) return 0;   // XSAVE and AVX
    cpuid(0xD, 0, &a, &b, &c, &d);
    return (uint64_t)a | ((uint64_t)d << 32);
}

static uint64_t wanted_xcr0(uint64_t supported) {
    const uint64_t base = XCR0_X87 | XCR0_SSE | XCR0_AVX;
    if ((supported & base) != base) return 0;
    uint64_t want = base;
    if (cpuid_max_leaf() >= 7) {
        uint32_t a, b, c, d;
        cpuid(7, 0, &a, &b, &c, &d);
        if ((b & (1u << 16)) && (supported & XCR0_AVX512) == XCR0_AVX512)
            want |= XCR0_AVX512;    // AVX-512F and all three of its state components
    }
    return want;
}

static void compute_features(void) {
    uint32_t a, b, c1, d, b7 = 0, ce = 0;
    cpuid(1, 0, &a, &b, &c1, &d);
    if (cpuid_max_leaf() >= 7) {
        uint32_t c7, d7;
        cpuid(7, 0, &a, &b7, &c7, &d7);
    }
    cpuid(0x80000000u, 0, &a, &b, &ce, &d);
    uint32_t max_ext = a;
    ce = 0;
    if (max_ext >= 0x80000001u) cpuid(0x80000001u, 0, &a, &b, &ce, &d);

    uint64_t f = 0;
    if (c1 & (1u << 0))  f |= SYS_CPU_SSE3;
    if (c1 & (1u << 9))  f |= SYS_CPU_SSSE3;
    if (c1 & (1u << 19)) f |= SYS_CPU_SSE41;
    if (c1 & (1u << 20)) f |= SYS_CPU_SSE42;
    if (c1 & (1u << 23)) f |= SYS_CPU_POPCNT;
    if (c1 & (1u << 25)) f |= SYS_CPU_AES;
    if (c1 & (1u << 1))  f |= SYS_CPU_PCLMUL;
    if (c1 & (1u << 22)) f |= SYS_CPU_MOVBE;
    if (c1 & (1u << 30)) f |= SYS_CPU_RDRAND;
    if (ce & (1u << 5))  f |= SYS_CPU_LZCNT;
    if (b7 & (1u << 3))  f |= SYS_CPU_BMI1;
    if (b7 & (1u << 8))  f |= SYS_CPU_BMI2;
    if (b7 & (1u << 18)) f |= SYS_CPU_RDSEED;
    if (b7 & (1u << 19)) f |= SYS_CPU_ADX;
    if (b7 & (1u << 29)) f |= SYS_CPU_SHA;

    // VEX/EVEX instructions fault unless the OS enabled the state: these
    // bits describe what a program can run, not what the CPU could do.
    const uint64_t ymm = XCR0_SSE | XCR0_AVX;
    if (simd_use_xsave && (simd_xcr0 & ymm) == ymm) {
        f |= SYS_CPU_AVX;
        if (c1 & (1u << 12)) f |= SYS_CPU_FMA;
        if (c1 & (1u << 29)) f |= SYS_CPU_F16C;
        if (b7 & (1u << 5))  f |= SYS_CPU_AVX2;
        if ((simd_xcr0 & XCR0_AVX512) == XCR0_AVX512 && (b7 & (1u << 16))) {
            f |= SYS_CPU_AVX512F;
            if (b7 & (1u << 17)) f |= SYS_CPU_AVX512DQ;
            if (b7 & (1u << 28)) f |= SYS_CPU_AVX512CD;
            if (b7 & (1u << 30)) f |= SYS_CPU_AVX512BW;
            if (b7 & (1u << 31)) f |= SYS_CPU_AVX512VL;
        }
    }
    g_cpu_features = f;
}

uint64_t cpu_simd_features(void) {
    return g_cpu_features;
}

void cpu_simd_init_bsp(multiboot2_info_t* mb_info) {
    kernel_boot_noavx = multiboot2_has_cmdline_token(mb_info, "noavx");

    uint64_t want = kernel_boot_noavx ? 0 : wanted_xcr0(supported_xcr0());
    if (want) {
        set_osxsave(true);
        write_xcr0(want);
        // The size CPUID reports is for the features enabled in XCR0 right now.
        uint32_t a, b, c, d;
        cpuid(0xD, 0, &a, &b, &c, &d);
        uint64_t size = ((uint64_t)b + 63u) & ~63ull;
        if (size < 576 || size > SIMD_MAX_SAVE_SIZE) {
            serial_write_str("CPU: odd XSAVE area size, staying on fxsave\n");
            write_xcr0(XCR0_X87 | XCR0_SSE);
            set_osxsave(false);
        } else {
            simd_xcr0 = want;
            simd_save_size = size;
            // The stubs read the switch last: everything they need is set.
            __atomic_store_n(&simd_use_xsave, 1, __ATOMIC_SEQ_CST);
        }
    }
    compute_features();

    serial_write_str("CPU: SIMD state saved with ");
    if (simd_use_xsave) {
        serial_write_str("xsave, XCR0=");
        serial_write_hex(simd_xcr0);
        serial_write_str(", area ");
        serial_write_dec(simd_save_size);
        serial_write_str(" bytes");
    } else {
        serial_write_str("fxsave");
        if (kernel_boot_noavx) serial_write_str(" (noavx)");
    }
    serial_write_str(", features ");
    serial_write_hex(g_cpu_features);
    serial_write_str("\n");
}

void cpu_simd_init_ap(uint32_t cpu_id) {
    if (!simd_use_xsave) return;
    uint64_t have = supported_xcr0();
    if ((have & simd_xcr0) != simd_xcr0) {
        // The stubs already use xsave with the boot CPU's mask, so there is
        // no way back; say so instead of triple-faulting silently.
        serial_write_str("CPU: cpu ");
        serial_write_dec(cpu_id);
        serial_write_str(" lacks the XSAVE features of the boot CPU\n");
        return;
    }
    set_osxsave(true);
    write_xcr0(simd_xcr0);
}
