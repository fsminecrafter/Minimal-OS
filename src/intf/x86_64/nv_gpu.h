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

// Reads the VBIOS through BAR0's PROM window (ctx = the adapter). The
// caller turns ROM shadowing off around it (NV_PCI_ROM_SHADOW bit 0).
bool nv_prom_read(void* ctx, uint64_t offset, void* buf, uint32_t len);

// Step 2b, first part: runs the VBIOS's FWSEC on the GSP falcon with the
// FRTS command, which sets up the protected WPR2 region GSP-RM boots in.
// Writes to the card; 'gpu nvidia boot'. Prints what it does.
bool nv_gsp_fwsec_frts(gpu_adapter_t* a, void (*print)(const char* text));

// WPR2 bounds as set by FWSEC-FRTS; false while there is none.
bool nv_gsp_wpr2(gpu_adapter_t* a, uint64_t* lo, uint64_t* hi);

// Step 2b, second part, and step 3a (nv_gsp_rm.c): after FWSEC-FRTS,
// runs booter_load on SEC2, which starts GSP-RM on the GSP core, waits
// for GSP_INIT_DONE and makes a first RPC. Writes to the card; prints
// what it does and saves it to 0:/gsp_boot.log.
bool nv_gsp_rm_boot(gpu_adapter_t* a, void (*print)(const char* text));
bool nv_gsp_rm_running(gpu_adapter_t* a);
bool nv_gsp_rm_rpc_ok(gpu_adapter_t* a);

// Prints the bring-up status of an NVIDIA adapter, line by line.
void nv_bringup_report(gpu_adapter_t* a, void (*print)(const char* text));
