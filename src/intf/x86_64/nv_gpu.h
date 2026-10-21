#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/gpucore.h"
#include "x86_64/nv_hw.h"

/*
 * Kernel side of NVIDIA hardware rendering: register access, falcons,
 * firmware files and the bring-up status ('gpu nvidia').
 */

static inline uint32_t nv_rd32(gpu_adapter_t* a, uint32_t reg) {
    if (!a->mmio || (uint64_t)reg + 4 > a->mmio_size) return 0xFFFFFFFFu;
    return a->mmio[reg / 4];
}

static inline void nv_wr32(gpu_adapter_t* a, uint32_t reg, uint32_t value) {
    if (!a->mmio || (uint64_t)reg + 4 > a->mmio_size) return;
    a->mmio[reg / 4] = value;
}

static inline uint16_t nv_chipset(gpu_adapter_t* a) {
    uint32_t boot0 = nv_rd32(a, NV_PMC_BOOT_0);
    return nv_pri_error(boot0) ? 0 : (uint16_t)((boot0 >> 20) & 0x1FF);
}

typedef struct {
    const char* name;
    uint32_t base;
} nv_falcon_t;

typedef struct {
    bool present;               // its registers answer
    uint32_t hwcfg, cpuctl, mailbox0, mailbox1;
    uint32_t imem_bytes, dmem_bytes;
    bool halted, stopped;
} nv_falcon_state_t;

// Reads a falcon's state. Read-only, safe on any card.
bool nv_falcon_read_state(gpu_adapter_t* a, const nv_falcon_t* f, nv_falcon_state_t* out);

/* Loading and starting falcon code (for GSP boot, step 2). Not called yet.
 * Data is padded to 256-byte IMEM blocks / 4-byte DMEM words. */
void nv_falcon_imem_write(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t dst,
                          const void* src, uint32_t bytes, uint32_t tag, bool secure);
void nv_falcon_dmem_write(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t dst,
                          const void* src, uint32_t bytes);
void nv_falcon_dmem_read(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t src,
                         void* dst, uint32_t bytes);
void nv_falcon_start(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t boot_vector);
// Polls until the falcon halts; false on timeout.
bool nv_falcon_wait_halted(gpu_adapter_t* a, const nv_falcon_t* f, uint32_t timeout_ms);

// The falcons the report shows for a chipset (NULL-name terminated).
const nv_falcon_t* nv_falcons(uint16_t chipset);

// Prints the bring-up status of an NVIDIA adapter, line by line.
void nv_bringup_report(gpu_adapter_t* a, void (*print)(const char* text));
