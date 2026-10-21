#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * NVIDIA GPU hardware definitions for running FGPU work on the card
 * (Turing first: RTX 20 / GTX 16). No kernel dependencies, so the
 * encoders and firmware parsers also build on the host
 * (tests/host/nv_hw_tests.c).
 *
 * Sources: nouveau (drm/nouveau/nvkm), NVIDIA's open-gpu-doc (class
 * headers clc597.h, clc46f.h, ...) and Mesa (nvc0_3d.xml). Nothing here
 * has been tried on a card yet; values marked "unverified" are the first
 * things to check when one is available.
 *
 * Bring-up order (docs/gpu.md, "NVIDIA cards and hardware rendering"):
 *   1. firmware files present and well-formed      nv_fw_*            (done)
 *   2. GSP boot: FWSEC-FRTS from the VBIOS, then booter_load on SEC2
 *      starts GSP-RM on the GSP RISC-V core          nv_falcon_* helpers (started)
 *   3. RM RPC: allocate a channel, GPFIFO, 3D class   nv_push_*, classes (encoders done)
 *   4. FGPU hardware backend: pushbuffers + SASS from fgpu_ir           (IR done)
 */

/* ----------------------------------------------------------- registers */

#define NV_PMC_BOOT_0           0x000000u   // chip id: bits 28:20 chipset
#define NV_PMC_ENABLE           0x000200u   // engine enables (pre-Ampere)

// PRI reads of a missing or powered-off unit return 0xBADxxxxx.
static inline bool nv_pri_error(uint32_t v) {
    return v == 0xFFFFFFFFu || (v & 0xFFF00000u) == 0xBAD00000u;
}

/* Falcons: NVIDIA's internal microcontrollers. Each has the same register
 * block at its own base. Turing's GSP is a RISC-V core but keeps a falcon
 * register block for loading and starting it. */
#define NV_FALCON_PMU           0x10A000u
#define NV_FALCON_FECS          0x409000u   // graphics context switching
#define NV_FALCON_GPCCS         0x41A000u
#define NV_FALCON_NVDEC         0x084000u
#define NV_FALCON_SEC2          0x840000u   // Pascal GP102 and later
#define NV_FALCON_GSP           0x110000u   // Turing and later

#define NV_FALCON_MAILBOX0      0x040u
#define NV_FALCON_MAILBOX1      0x044u
#define NV_FALCON_CPUCTL        0x100u
#define   NV_FALCON_CPUCTL_STARTCPU (1u << 1)
#define   NV_FALCON_CPUCTL_HALTED   (1u << 4)
#define   NV_FALCON_CPUCTL_STOPPED  (1u << 5)
#define   NV_FALCON_CPUCTL_ALIAS_EN (1u << 6)   // start through CPUCTL_ALIAS
#define NV_FALCON_BOOTVEC       0x104u
#define NV_FALCON_HWCFG         0x108u      // bits 8:0 IMEM / 256, bits 17:9 DMEM / 256
#define NV_FALCON_DMACTL        0x10Cu
#define NV_FALCON_CPUCTL_ALIAS  0x130u
#define NV_FALCON_IMEMC(port)   (0x180u + (port) * 16u)
#define NV_FALCON_IMEMD(port)   (0x184u + (port) * 16u)
#define NV_FALCON_IMEMT(port)   (0x188u + (port) * 16u)
#define NV_FALCON_DMEMC(port)   (0x1C0u + (port) * 8u)
#define NV_FALCON_DMEMD(port)   (0x1C4u + (port) * 8u)
#define   NV_FALCON_MEMC_AINCW  (1u << 24)  // address increments on write
#define   NV_FALCON_MEMC_AINCR  (1u << 25)  // ... on read
#define   NV_FALCON_IMEMC_SECURE (1u << 28)

static inline uint32_t nv_falcon_imem_bytes(uint32_t hwcfg) { return (hwcfg & 0x1FFu) << 8; }
static inline uint32_t nv_falcon_dmem_bytes(uint32_t hwcfg) { return ((hwcfg >> 9) & 0x1FFu) << 8; }

/* ------------------------------------------------------------- classes */

#define NV_CLASS_PASCAL_A               0xC097u
#define NV_CLASS_PASCAL_B               0xC197u
#define NV_CLASS_VOLTA_A                0xC397u
#define NV_CLASS_TURING_A               0xC597u     // 3D
#define NV_CLASS_TURING_COMPUTE_A       0xC5C0u
#define NV_CLASS_TURING_DMA_COPY_A      0xC5B5u
#define NV_CLASS_TURING_CHANNEL_GPFIFO_A 0xC46Fu
#define NV_CLASS_VOLTA_USERMODE_A       0xC361u     // doorbell page
#define NV_USERMODE_NOTIFY_CHANNEL_PENDING 0x90u    // write the channel's work token here

/* --------------------------------------------------------- pushbuffers */

/* A channel executes pushbuffers: method headers followed by data words.
 * Header (Fermi and later): bits 31:29 type, 28:16 count (or immediate
 * data), 15:13 subchannel, 11:0 method address / 4. */
#define NV_PUSH_TYPE_INC     1u     // data goes to method, method + 4, ...
#define NV_PUSH_TYPE_NONINC  3u     // all data to the same method
#define NV_PUSH_TYPE_IMMD    4u     // 13-bit data in the header, no data words
#define NV_PUSH_TYPE_ONEINC  5u     // first word to method, the rest to method + 4

static inline uint32_t nv_push_header(uint32_t type, uint32_t subc, uint32_t method, uint32_t count) {
    return (type << 29) | ((count & 0x1FFFu) << 16) | ((subc & 7u) << 13) | ((method >> 2) & 0xFFFu);
}

typedef struct {
    uint32_t* words;
    uint32_t capacity;      // words
    uint32_t count;
    bool overflow;          // a method did not fit; the buffer must not be submitted
} nv_push_t;

void nv_push_init(nv_push_t* p, uint32_t* words, uint32_t capacity);
// Incrementing method with `count` data words (count <= 8191).
void nv_push_method(nv_push_t* p, uint32_t subc, uint32_t method, const uint32_t* data, uint32_t count);
static inline void nv_push_1(nv_push_t* p, uint32_t subc, uint32_t method, uint32_t value) {
    nv_push_method(p, subc, method, &value, 1);
}
// Immediate method: value must fit in 13 bits.
void nv_push_immd(nv_push_t* p, uint32_t subc, uint32_t method, uint32_t value);

/* GPFIFO: the ring of pushbuffer pointers a channel reads. Each entry is
 * two words: GPU virtual address bits 31:2, then bits 39:32 | length in
 * words << 10. */
void nv_gpfifo_entry(uint64_t gpu_va, uint32_t words, uint32_t out[2]);

/* 3D class methods used by the first test (a clear). From Mesa's
 * nvc0_3d.xml, which matches the Fermi-to-Turing 3D classes for these;
 * unverified on Turing hardware. */
#define NV_3D_SET_OBJECT            0x0000u
#define NV_3D_RT_ADDRESS_HIGH(i)    (0x0800u + (i) * 0x40u)
#define NV_3D_RT_ADDRESS_LOW(i)     (0x0804u + (i) * 0x40u)
#define NV_3D_RT_HORIZ(i)           (0x0808u + (i) * 0x40u)    // linear: pitch in bytes
#define NV_3D_RT_VERT(i)            (0x080Cu + (i) * 0x40u)
#define NV_3D_RT_FORMAT(i)          (0x0810u + (i) * 0x40u)
#define NV_3D_RT_TILE_MODE(i)       (0x0814u + (i) * 0x40u)
#define   NV_3D_RT_TILE_MODE_LINEAR 0x1000u
#define NV_3D_RT_ARRAY_MODE(i)      (0x0818u + (i) * 0x40u)
#define NV_3D_RT_LAYER_STRIDE(i)    (0x081Cu + (i) * 0x40u)
#define NV_3D_SCREEN_SCISSOR_HORIZ  0x0FF4u                     // width << 16 | x
#define NV_3D_SCREEN_SCISSOR_VERT   0x0FF8u
#define NV_3D_RT_CONTROL            0x121Cu                     // count | map << 4
#define NV_3D_CLEAR_COLOR(i)        (0x1590u + (i) * 4u)        // r, g, b, a floats
#define NV_3D_CLEAR_BUFFERS         0x19D0u
#define   NV_3D_CLEAR_BUFFERS_RGBA  0x3Cu                       // | rt << 6
#define NV_RT_FORMAT_BGRA8_UNORM    0xCFu
#define NV_RT_FORMAT_BGRX8_UNORM    0xE6u

// Binds the 3D class to subchannel 0 and clears a linear XRGB8888
// surface at gpu_va. The first thing to submit once a channel works.
void nv_push_clear_xrgb(nv_push_t* p, uint64_t gpu_va, uint32_t width, uint32_t height,
                        uint32_t pitch_bytes, uint32_t xrgb);

/* ------------------------------------------------------------ firmware */

/* NVIDIA's signed firmware, as published in linux-firmware
 * (nvidia/<chip>/...), copied unchanged to 0:/firmware/nvidia/<chip>/. */

#define NV_FW_DIR "0:/firmware/nvidia/"
#define NV_GSP_FW_VERSION "535.113.01"      // the release nouveau's GSP support uses

// Header of NVIDIA's ".bin" firmware containers (booter, bootloader, ACR).
typedef struct {
    uint32_t bin_magic;         // NV_FW_BIN_MAGIC
    uint32_t bin_ver;
    uint32_t bin_size;          // whole file
    uint32_t header_offset;     // the image-specific descriptor
    uint32_t data_offset;       // the ucode
    uint32_t data_size;
} nv_fw_bin_hdr_t;

#define NV_FW_BIN_MAGIC 0x10DEu

// Reads `len` bytes at `offset`; false on error. Lets the parsers work on
// a file without loading it whole (the GSP image is about 30 MB).
typedef bool (*nv_fw_read_fn)(void* ctx, uint64_t offset, void* buf, uint32_t len);

typedef struct {
    nv_fw_read_fn read;
    void* ctx;
    uint64_t size;
} nv_fw_file_t;

bool nv_fw_parse_bin(const nv_fw_file_t* f, nv_fw_bin_hdr_t* out, const char** why);

// Finds a section of an ELF64 file (the GSP-RM image) by name.
bool nv_fw_elf_section(const nv_fw_file_t* f, const char* name, uint64_t* offset, uint64_t* size,
                       const char** why);

typedef enum {
    NV_FW_KIND_BIN,             // nv_fw_bin_hdr_t container
    NV_FW_KIND_ELF,             // ELF with the sections below
} nv_fw_kind_t;

typedef struct {
    const char* file;           // under NV_FW_DIR "<chip>/"
    nv_fw_kind_t kind;
    const char* purpose;
    const char* sections[2];    // ELF: sections that must exist (NULL = unused)
} nv_fw_entry_t;

// "tu106" for chipset 0x166 and so on; NULL for chips without a firmware list yet.
const char* nv_chip_name(uint16_t chipset);

// The firmware files `chipset` needs for hardware rendering; NULL (and
// *count = 0) when none is listed for it yet.
const nv_fw_entry_t* nv_fw_manifest(uint16_t chipset, uint32_t* count);
