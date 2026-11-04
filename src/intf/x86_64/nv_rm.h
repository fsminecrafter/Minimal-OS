#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "x86_64/nv_hw.h"

/*
 * GSP-RM boot and RPC formats for NVIDIA Turing (step 2b, second part,
 * and step 3a). Layouts are those of the 535.113.01 firmware, as used by
 * NVIDIA's open-gpu-kernel-modules and nouveau (nvkm/subdev/gsp/r535.c).
 * No kernel dependencies: also built for tests/host/nv_hw_tests.c.
 *
 * Boot, in order:
 *   FWSEC-FRTS (VBIOS, on the GSP falcon) has set up WPR2's FRTS part.
 *   The CPU lays out sysmem: the GSP-RM ELF image behind a 3-level page
 *   table ("radix3"), the RISC-V boot loader, its signature, the LibOS
 *   arguments (log buffers + RM arguments with the message queues) and
 *   GspFwWprMeta describing where everything goes in VRAM.
 *   Two RPCs are queued before boot: GSP_SET_SYSTEM_INFO, SET_REGISTRY.
 *   booter_load (signed HS ucode) runs on SEC2 with the GspFwWprMeta
 *   address in its mailboxes: it copies and checks the images into WPR2
 *   and starts the boot loader on the GSP RISC-V core, which starts GSP-RM.
 *   GSP-RM reads its arguments through the GSP mailboxes (LibOS args)
 *   and answers with GSP_INIT_DONE on the message queue.
 */

/* ----------------------------------------------------- booter (HS v2) */

// The file layout of booter_load/booter_unload: nv_fw_bin_hdr_t, then a
// "HS header v2" whose fields are file offsets (some of them offsets of
// a u32 holding the value).
typedef struct {
    uint32_t data_offset, data_size;    // ucode image in the file
    uint32_t sig_offset, sig_size;      // file offset of the first production signature, bytes each
    uint32_t sig_count;
    uint32_t patch_loc;                 // image offset the chosen signature is copied to
    uint32_t os_code_offset, os_code_size;  // non-secure code (image offsets)
    uint32_t os_data_offset, os_data_size;  // DMEM image
    uint32_t app_offset, app_size;      // secure (HS) code, entry point = app_offset
    uint32_t meta[3];                   // fuse version, engine id mask, ucode id
} nv_booter_t;

bool nv_fw_parse_booter(const nv_fw_file_t* f, nv_booter_t* out, const char** why);

/* ----------------------------------------------- GSP RISC-V boot loader */

// RM_RISCV_UCODE_DESC (version 4) at the bin header's header_offset.
typedef struct {
    uint32_t data_offset, data_size;    // the boot loader image in the file
    uint32_t version;
    uint32_t code_offset, data_off, manifest_offset;   // monitor code/data, manifest
    uint32_t app_version;
} nv_rm_bootloader_t;

bool nv_fw_parse_rm_bootloader(const nv_fw_file_t* f, nv_rm_bootloader_t* out, const char** why);

/* ------------------------------------------------------ VRAM layout */

typedef struct {
    uint64_t fb_size;
    uint64_t vga_addr, vga_size;        // VBIOS/VGA workspace at the top of VRAM
    uint64_t frts_addr, frts_size;      // set up by FWSEC-FRTS
    uint64_t boot_addr, boot_size;      // RISC-V boot loader copy
    uint64_t elf_addr, elf_size;        // GSP-RM image copy
    uint64_t heap_addr, heap_size;      // GSP-RM's heap
    uint64_t wpr2_addr, wpr2_end;       // the whole protected region
    uint64_t nonwpr_addr, nonwpr_size;  // unprotected heap below it
} nv_gsp_layout_t;

// Turing (LibOS 2) heap sizing: base + per-GB + client allocations,
// at least 64 MiB.
#define NV_GSP_HEAP_BASE        (8ull << 20)
#define NV_GSP_HEAP_PER_GB      (96ull << 10)
#define NV_GSP_HEAP_CLIENT      ((48ull << 10) * 2048)
#define NV_GSP_HEAP_MIN         (64ull << 20)

void nv_gsp_layout(nv_gsp_layout_t* l, uint64_t fb_size, uint64_t vga_addr,
                   uint64_t boot_size, uint64_t elf_size);

/* VBIOS workspace from the display's VGA register (0x625F04): bit 3
 * valid, bits 31:8 address >> 16. Falls back to the top 128 KiB. */
#define NV_PDISP_VGA_WORKSPACE  0x625F04u
uint64_t nv_gsp_vga_workspace(uint32_t reg, uint64_t fb_size);

/* VRAM size from 0x100CE0 (Pascal..Ampere): (bits 9:4) << (bits 3:0 + 20),
 * 15/16 of that when bit 30 is set (ECC). */
#define NV_PFB_VIDMEM_SIZE      0x100CE0u
uint64_t nv_vidmem_size(uint32_t reg);

/* ------------------------------------------------- GspFwWprMeta (256 B) */

#define NV_GSP_WPR_META_MAGIC    0xDC3AAE21371A60B3ull
#define NV_GSP_WPR_META_REVISION 1ull
#define NV_GSP_WPR_META_VERIFIED 0xA0A0A0A0A0A0A0A0ull

typedef struct {
    uint64_t magic, revision;
    uint64_t sysmemAddrOfRadix3Elf, sizeOfRadix3Elf;
    uint64_t sysmemAddrOfBootloader, sizeOfBootloader;
    uint64_t bootloaderCodeOffset, bootloaderDataOffset, bootloaderManifestOffset;
    uint64_t sysmemAddrOfSignature, sizeOfSignature;
    uint64_t gspFwRsvdStart;
    uint64_t nonWprHeapOffset, nonWprHeapSize;
    uint64_t gspFwWprStart;
    uint64_t gspFwHeapOffset, gspFwHeapSize;
    uint64_t gspFwOffset;
    uint64_t bootBinOffset;
    uint64_t frtsOffset, frtsSize;
    uint64_t gspFwWprEnd;
    uint64_t fbSize;
    uint64_t vgaWorkspaceOffset, vgaWorkspaceSize;
    uint64_t bootCount;
    uint64_t partitionRpcAddr;
    uint16_t partitionRpcRequestOffset, partitionRpcReplyOffset;
    uint32_t elfCodeOffset, elfDataOffset, elfCodeSize, elfDataSize;
    uint32_t lsUcodeVersion;
    uint8_t gspFwHeapVfPartitionCount;
    uint8_t padding[7];
    uint64_t verified;
} nv_gsp_wpr_meta_t;

_Static_assert(sizeof(nv_gsp_wpr_meta_t) == 256, "GspFwWprMeta is 256 bytes");
_Static_assert(offsetof(nv_gsp_wpr_meta_t, gspFwRsvdStart) == 88, "GspFwWprMeta layout");
_Static_assert(offsetof(nv_gsp_wpr_meta_t, bootCount) == 200, "GspFwWprMeta layout");
_Static_assert(offsetof(nv_gsp_wpr_meta_t, verified) == 248, "GspFwWprMeta layout");

void nv_gsp_fill_wpr_meta(nv_gsp_wpr_meta_t* m, const nv_gsp_layout_t* l);

/* --------------------------------------------------------- radix3 */

// GSP-RM's image is passed as a 3-level table of 4 KiB page addresses:
// level 0 (one page) lists the level-1 pages, level 1 the level-2 pages,
// level 2 the image's pages. Returns the number of 4 KiB pages level
// `lvl` needs for an image of `bytes`.
uint32_t nv_radix3_pages(uint64_t bytes, int lvl);

/* ------------------------------------------------------------ LibOS */

typedef struct {
    uint64_t id8;       // name, up to 8 characters, big-endian packed
    uint64_t pa;
    uint64_t size;
    uint8_t kind;       // 1 = contiguous
    uint8_t loc;        // 1 = sysmem
    uint8_t pad[6];
} nv_libos_region_t;

_Static_assert(sizeof(nv_libos_region_t) == 32, "LibosMemoryRegionInitArgument");

#define NV_LIBOS_KIND_CONTIGUOUS 1u
#define NV_LIBOS_LOC_SYSMEM      1u
#define NV_GSP_LOG_SIZE          0x10000u

uint64_t nv_libos_id8(const char* name);

/* ---------------------------------------------------- message queues */

typedef struct {
    uint32_t version, size, msgSize, msgCount;
    uint32_t writePtr, flags, rxHdrOff, entryOff;
} nv_msgq_tx_t;

typedef struct {
    nv_msgq_tx_t tx;
    uint32_t readPtr;   // the *other* queue's read position
} nv_msgq_hdr_t;

#define NV_GSP_QUEUE_SIZE   0x40000u
#define NV_GSP_PAGE         0x1000u

// GSP_ARGUMENTS_CACHED.
typedef struct {
    uint64_t sharedMemPhysAddr;
    uint32_t pageTableEntryCount;
    uint32_t pad0;
    uint64_t cmdQueueOffset, statQueueOffset;
    uint64_t locklessCmdQueueOffset, locklessStatQueueOffset;
    uint32_t srOldLevel, srFlags;
    uint8_t srInPMTransition;
    uint8_t pad1[3];
    uint32_t gpuInstance;
    uint64_t profilerPa, profilerSize;
} nv_gsp_rmargs_t;

_Static_assert(sizeof(nv_gsp_rmargs_t) == 80, "GSP_ARGUMENTS_CACHED");

/* ------------------------------------------------------------- RPCs */

// One queue entry: 48-byte transport header, then the RPC.
typedef struct {
    uint8_t authTag[16];
    uint8_t aad[16];
    uint32_t checksum;
    uint32_t seqNum;
    uint32_t elemCount;     // 4 KiB queue slots this message uses
    uint32_t pad;
} nv_gsp_msg_t;

typedef struct {
    uint32_t header_version;    // 0x03000000
    uint32_t signature;         // "VRPC"
    uint32_t length;            // this header + payload
    uint32_t function;
    uint32_t rpc_result;        // 0xFFFFFFFF while pending
    uint32_t rpc_result_private;
    uint32_t sequence;
    uint32_t spare;
} nv_rpc_hdr_t;

_Static_assert(sizeof(nv_gsp_msg_t) == 48, "GSP_MSG_QUEUE_ELEMENT header");
_Static_assert(sizeof(nv_rpc_hdr_t) == 32, "rpc_message_header_v03_00");

#define NV_RPC_HEADER_VERSION   0x03000000u
#define NV_RPC_SIGNATURE        0x43505256u     // 'V' 'R' 'P' 'C'
#define NV_RPC_RESULT_PENDING   0xFFFFFFFFu

#define NV_RPC_GET_GSP_STATIC_INFO   65u
#define NV_RPC_GSP_SET_SYSTEM_INFO   72u
#define NV_RPC_SET_REGISTRY          73u
#define NV_RPC_EVENT_GSP_INIT_DONE           0x1001u
#define NV_RPC_EVENT_RUN_CPU_SEQUENCER       0x1002u
#define NV_RPC_EVENT_POST_EVENT              0x1003u
#define NV_RPC_EVENT_RC_TRIGGERED            0x1004u
#define NV_RPC_EVENT_MMU_FAULT_QUEUED        0x1005u
#define NV_RPC_EVENT_OS_ERROR_LOG            0x1006u
#define NV_RPC_EVENT_UCODE_LIBOS_PRINT       0x100Cu

const char* nv_rpc_name(uint32_t function);

// XOR of the message's 64-bit words, folded to 32 bits (checksum field
// zero while computing). `bytes` is rounded up to 8; the tail must be 0.
uint32_t nv_gsp_checksum(const void* msg, uint32_t bytes);

// Queue slots (4 KiB) a message of `bytes` (header included) needs.
static inline uint32_t nv_gsp_msg_slots(uint32_t bytes) {
    return (bytes + NV_GSP_PAGE - 1) / NV_GSP_PAGE;
}

// SET_REGISTRY payload (PACKED_REGISTRY_TABLE) with DWORD entries;
// returns its size, 0 if it does not fit in `cap`.
typedef struct { const char* name; uint32_t value; } nv_reg_entry_t;
uint32_t nv_gsp_pack_registry(uint8_t* out, uint32_t cap, const nv_reg_entry_t* e, uint32_t count);

/* GSP_SET_SYSTEM_INFO payload: the first fields of GspSystemInfo. The
 * rest (bridge and ACPI data, hypervisor) stays zero; the payload sent
 * is padded with zeros to NV_GSP_SYSINFO_BYTES. */
typedef struct {
    uint64_t gpuPhysAddr;           // BAR0
    uint64_t gpuPhysFbAddr;         // BAR1
    uint64_t gpuPhysInstAddr;       // BAR3
    uint64_t nvDomainBusDeviceFunc;
    uint64_t simAccessBufPhysAddr;
    uint64_t pcieAtomicsOpMask;
    uint64_t consoleMemSize;
    uint64_t maxUserVa;
    uint32_t pciConfigMirrorBase;
    uint32_t pciConfigMirrorSize;
} nv_gsp_sysinfo_t;

#define NV_GSP_SYSINFO_BYTES 0xE00u

/* RUN_CPU_SEQUENCER: commands GSP-RM asks the CPU to run. Payload:
 * bufferSizeDWord, cmdIndex (dwords used), regSaveArea[8], commands. */
#define NV_SEQ_REG_WRITE        0u  // addr, value
#define NV_SEQ_REG_MODIFY       1u  // addr, mask, value
#define NV_SEQ_REG_POLL         2u  // addr, mask, value, timeout us, error
#define NV_SEQ_DELAY_US         3u  // us
#define NV_SEQ_REG_STORE        4u  // addr, save index
#define NV_SEQ_CORE_RESET       5u
#define NV_SEQ_CORE_START       6u
#define NV_SEQ_CORE_WAIT_HALT   7u
#define NV_SEQ_CORE_RESUME      8u

// Payload dwords after the opcode; -1 for an unknown opcode.
int nv_seq_payload_dwords(uint32_t opcode);

/* ------------------------------------------------- falcon registers */

#define NV_FALCON_IRQSCLR       0x004u
#define NV_FALCON_OS            0x080u      // GSP: boot loader app version
#define NV_FALCON_DEBUG1        0x090u
#define NV_FALCON_SEC2_DEBUG    0x408u      // bit 20: debug-fused part
#define NV_FALCON_ENGINE        0x3C0u      // bit 0: engine reset
#define NV_FALCON_FBIF_CTL      0x624u      // bit 7: allow physical DMA without a context
#define NV_FALCON_GSP_QUEUE_HEAD0 0xC00u    // GSP command queue doorbell
#define NV_FALCON_RISCV_CORE_STATUS 0x1240u // Turing GSP RISC-V: bit 0 active
#define NV_PTIMER_TIME_0        0x009400u   // ns, low word
#define NV_PTIMER_TIME_1        0x009410u
