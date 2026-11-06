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

/* ------------------------------------------ FWSEC-FRTS (Turing, v2) */

/* Step 2b, first part (nv_gsp.c). Turing's FWSEC (VBIOS descriptor v2)
 * is not loaded into the GSP falcon directly: the ACR boot loader
 * (acr/bl.bin) goes into the top of IMEM, a descriptor at DMEM 0 tells it
 * where the FWSEC image is in sysmem, and it DMAs the image in and runs
 * it. The image is patched first so FWSEC carries out the FRTS command:
 * set up the 1 MiB FRTS region and the WPR2 protection around it. As
 * nouveau does it (gsp/fwsec.c, gsp/tu102.c, falcon/gm200.c). */

// acr/bl.bin: nv_fw_bin_hdr_t, then this at the header offset.
typedef struct {
    uint32_t code_offset, code_size;    // boot loader code in the file
    uint32_t start_tag;                 // IMEM tag; boot vector = start_tag << 8
    uint32_t dmem_load_off;
} nv_acr_bl_t;

bool nv_fw_parse_acr_bl(const nv_fw_file_t* f, nv_acr_bl_t* out, const char** why);

// What the boot loader finds at DMEM 0 (flcn_bl_dmem_desc_v2).
#define NV_FALCON_DMAIDX_PHYS_SYS_NCOH 4u
typedef struct __attribute__((packed)) {
    uint32_t reserved[4];
    uint32_t signature[4];
    uint32_t ctx_dma;                   // NV_FALCON_DMAIDX_*
    uint64_t code_dma_base;             // sysmem address of the image
    uint32_t non_sec_code_off, non_sec_code_size;
    uint32_t sec_code_off, sec_code_size;
    uint32_t code_entry_point;
    uint64_t data_dma_base;             // sysmem address of the DMEM part
    uint32_t data_size;
    uint32_t argc, argv;
} nv_bl_dmem_desc_t;

_Static_assert(sizeof(nv_bl_dmem_desc_t) == 84, "flcn_bl_dmem_desc_v2 is 84 bytes");

// The descriptor for FWSEC's image (`v` from nv_vbios_parse) at `image_pa`.
void nv_fwsec_bl_desc(nv_bl_dmem_desc_t* d, const nv_vbios_info_t* v, uint64_t image_pa);

// FRTS command arguments, written into the DMEM mapper's command buffer
// (nvfw_fwsec_frts_cmd): no VBIOS image passed in, and the FRTS region
// in VRAM in 4 KiB units.
#define NV_FWSEC_FRTS_CMD_BYTES 44u
#define NV_FWSEC_FRTS_REGION_FB 2u

// Patches FWSEC's ucode image (`image`, v->stored_size bytes, as read
// from the ROM at v->image_offset) to run the FRTS command for the
// region at `frts_addr`. False (with *why) if the mapper cannot take it.
bool nv_fwsec_patch_frts(uint8_t* image, const nv_vbios_info_t* v, uint64_t frts_addr,
                         uint64_t frts_size, const char** why);

/* WPR2, set up by FWSEC-FRTS. Each register holds address bits 39:12 in
 * bits 31:4; the low bound is inclusive, the high bound exclusive. */
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO 0x1FA824u
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI 0x1FA828u
// False when there is no WPR2 (the high bound is 0, or the reads failed).
bool nv_wpr2_decode(uint32_t lo_reg, uint32_t hi_reg, uint64_t* lo, uint64_t* hi);

// FWSEC's result: bits 31:16 of SW_SCRATCH 0x0E, 0 on success.
#define NV_PBUS_SW_SCRATCH_0E      0x001438u
// The VBIOS's own boot (GFW) must have finished before FWSEC runs:
// bit 0 of the priv level mask is set and the progress byte is 0xFF.
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK 0x118128u
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT      0x118234u
// A falcon's DMA aperture settings, one per DMA index.
#define NV_FALCON_FBIF_TRANSCFG(i) (0x600u + (i) * 4u)

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
// LibOS log buffers. Sizes must match the firmware's log sections
// (.section_log_init_buffer_instance 64 KiB, .section_log_rm_buffer_instance
// 256 KiB) and nouveau's r535_gsp_libos_init; a short LOGRM stops the
// init task right after it maps RMARGS (GSP mailbox 0xCA).
#define NV_GSP_LOGINIT_SIZE      0x10000u
#define NV_GSP_LOGINTR_SIZE      0x10000u
#define NV_GSP_LOGRM_SIZE        0x40000u

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
#define NV_RPC_EVENT_GSP_SEND_USER_SHARED_DATA 0x101Bu

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

/* ===================================================================
 * Step 3b: RM objects, GPU page tables and a copy-engine channel.
 *
 * Layouts from the 535.113.01 SDK headers nouveau ships
 * (include/nvrm/535.113.01/...), the order from nouveau's r535 code
 * (gsp/r535.c client/device, mmu/r535.c VA space, fifo/r535.c channel,
 * ce/r535.c copy engine object). GSP-RM owns the hardware state; we
 * own the memory: GPU page tables, the channel's instance block and
 * USERD (in VRAM), its GPFIFO ring and pushbuffers (in sysmem).
 * =================================================================== */

#define NV_RPC_FREE              10u
#define NV_RPC_CONTINUATION      71u
#define NV_RPC_GSP_RM_CONTROL    76u
#define NV_RPC_GSP_RM_ALLOC      103u

// rpc_gsp_rm_alloc_v03_00; the class's parameters follow.
typedef struct {
    uint32_t hClient, hParent, hObject, hClass;
    uint32_t status;
    uint32_t paramsSize;
    uint32_t flags;
    uint8_t reserved[4];
} nv_rm_alloc_rpc_t;

// rpc_gsp_rm_control_v03_00; the command's parameters follow (and come
// back in the reply).
typedef struct {
    uint32_t hClient, hObject, cmd;
    uint32_t status;
    uint32_t paramsSize;
    uint32_t flags;
} nv_rm_control_rpc_t;

// rpc_free_v03_00 (NVOS00_PARAMETERS_v03_00).
typedef struct {
    uint32_t hRoot, hObjectParent, hObjectOld, status;
} nv_rm_free_rpc_t;

_Static_assert(sizeof(nv_rm_alloc_rpc_t) == 32, "rpc_gsp_rm_alloc_v03_00");
_Static_assert(sizeof(nv_rm_control_rpc_t) == 24, "rpc_gsp_rm_control_v03_00");

/* Classes and their allocation parameters. */
#define NV_CLASS_ROOT            0x0000u    // NV01_ROOT (a client)
#define NV_CLASS_DEVICE          0x0080u    // NV01_DEVICE_0
#define NV_CLASS_SUBDEVICE       0x2080u    // NV20_SUBDEVICE_0
#define NV_CLASS_VASPACE         0x90F1u    // FERMI_VASPACE_A

typedef struct {                // NV0000_ALLOC_PARAMETERS
    uint32_t hClient;
    uint32_t processID;
    char processName[100];
} nv_root_alloc_t;

typedef struct {                // NV0080_ALLOC_PARAMETERS
    uint32_t deviceId;
    uint32_t hClientShare, hTargetClient, hTargetDevice;
    uint32_t flags;
    uint32_t pad0;
    uint64_t vaSpaceSize, vaStartInternal, vaLimitInternal;
    uint32_t vaMode;
    uint32_t pad1;
} nv_device_alloc_t;

typedef struct {                // NV2080_ALLOC_PARAMETERS
    uint32_t subDeviceId;
} nv_subdevice_alloc_t;

typedef struct {                // NV_VASPACE_ALLOCATION_PARAMETERS
    uint32_t index;             // 0 = NV_VASPACE_ALLOCATION_INDEX_GPU_NEW
    uint32_t flags;
    uint64_t vaSize, vaStartInternal, vaLimitInternal;
    uint32_t bigPageSize;
    uint32_t pad;
    uint64_t vaBase;
} nv_vaspace_alloc_t;

typedef struct {                // NV_MEMORY_DESC_PARAMS
    uint64_t base, size;
    uint32_t addressSpace;      // 1 sysmem, 2 VRAM
    uint32_t cacheAttrib;
} nv_mem_desc_t;

#define NV_ADDR_SYSMEM 1u
#define NV_ADDR_FBMEM  2u

typedef struct {                // NV_CHANNEL_ALLOC_PARAMS
    uint32_t hObjectError, hObjectBuffer;
    uint64_t gpFifoOffset;      // GPU VA of the GPFIFO ring
    uint32_t gpFifoEntries;
    uint32_t flags;             // NVOS04_FLAGS_*
    uint32_t hContextShare, hVASpace;
    uint32_t hUserdMemory[8];
    uint64_t userdOffset[8];
    uint32_t engineType;        // NV2080_ENGINE_TYPE_*
    uint32_t cid, subDeviceId, hObjectEccError;
    nv_mem_desc_t instanceMem, userdMem, ramfcMem, mthdbufMem;
    uint32_t hPhysChannelGroup;
    uint32_t internalFlags;
    nv_mem_desc_t errorNotifierMem, eccErrorNotifierMem;
    uint32_t ProcessID, SubProcessID;
    uint32_t encryptIv[3], decryptIv[3], hmacNonce[8];
} nv_channel_alloc_t;

_Static_assert(sizeof(nv_root_alloc_t) == 108, "NV0000_ALLOC_PARAMETERS");
_Static_assert(sizeof(nv_device_alloc_t) == 56, "NV0080_ALLOC_PARAMETERS");
_Static_assert(sizeof(nv_vaspace_alloc_t) == 48, "NV_VASPACE_ALLOCATION_PARAMETERS");
_Static_assert(sizeof(nv_mem_desc_t) == 24, "NV_MEMORY_DESC_PARAMS");
_Static_assert(offsetof(nv_channel_alloc_t, engineType) == 128, "NV_CHANNEL_ALLOC_PARAMS");
_Static_assert(offsetof(nv_channel_alloc_t, instanceMem) == 144, "NV_CHANNEL_ALLOC_PARAMS");
_Static_assert(offsetof(nv_channel_alloc_t, hPhysChannelGroup) == 240, "NV_CHANNEL_ALLOC_PARAMS");
_Static_assert(offsetof(nv_channel_alloc_t, ProcessID) == 296, "NV_CHANNEL_ALLOC_PARAMS");
_Static_assert(sizeof(nv_channel_alloc_t) == 360, "NV_CHANNEL_ALLOC_PARAMS");

// NVOS04_FLAGS (alloc_channel.h), the bits nouveau sets.
#define NVOS04_FLAGS_PRIVILEGED_CHANNEL        (1u << 5)
#define NVOS04_FLAGS_USERD_INDEX(i)            (((i) & 7u) << 8)
#define NVOS04_FLAGS_USERD_INDEX_PAGE(p)       (((p) & 0x1FFu) << 12)
#define NVOS04_FLAGS_USERD_INDEX_PAGE_FIXED    (1u << 21)
// NV_KERNELCHANNEL_ALLOC_INTERNALFLAGS: privilege 1:0 (1 = admin),
// error notifier type 3:2 and ECC notifier type 5:4 (1 = none).
#define NV_CHANNEL_INTERNAL_ADMIN_NO_NOTIFIERS (1u | (1u << 2) | (1u << 4))

typedef struct {                // NVC0B5_ALLOCATION_PARAMETERS (copy engine object)
    uint32_t version;           // 1
    uint32_t engineType;
} nv_ce_alloc_t;

/* RM controls used. */
#define NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE      0x20801112u
#define NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE 0x20802A08u
#define NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES 0x90F10106u
#define NVA06F_CTRL_CMD_GPFIFO_SCHEDULE                 0xA06F0103u
#define NVA06F_CTRL_CMD_BIND                            0xA06F0104u

typedef struct {                // NV2080_CTRL_FIFO_DEVICE_ENTRY
    uint32_t engineData[16];    // indexed by NV_ENGINE_INFO_*
    uint32_t pbdmaIds[2];
    uint32_t pbdmaFaultIds[2];
    uint32_t numPbdmas;
    char engineName[16];
} nv_fifo_device_entry_t;

typedef struct {                // NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS
    uint32_t baseIndex;
    uint32_t numEntries;
    uint8_t bMore;
    uint8_t pad[3];
    nv_fifo_device_entry_t entries[32];
} nv_fifo_device_info_t;

_Static_assert(sizeof(nv_fifo_device_entry_t) == 100, "NV2080_CTRL_FIFO_DEVICE_ENTRY");
_Static_assert(sizeof(nv_fifo_device_info_t) == 3212, "NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS");

// ENGINE_INFO_TYPE (g_kernel_fifo_nvoc.h)
#define NV_ENGINE_INFO_ENG_DESC         0u
#define NV_ENGINE_INFO_RM_ENGINE_TYPE   2u
#define NV_ENGINE_INFO_RUNLIST          3u
#define NV_ENGINE_INFO_RUNLIST_PRI_BASE 11u

// RM_ENGINE_TYPE and NV2080_ENGINE_TYPE agree on these.
#define NV_ENGINE_GR0    0x01u
#define NV_ENGINE_COPY0  0x09u
#define NV_ENGINE_COPY9  0x12u

typedef struct {                // NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS
    uint32_t hSubDevice;
    uint32_t subDeviceId;
    uint64_t pageSize;
    uint64_t virtAddrLo, virtAddrHi;
    uint32_t numLevelsToCopy;
    uint32_t pad;
    struct {
        uint64_t physAddress;
        uint64_t size;
        uint32_t aperture;      // 1 = VRAM (GMMU_APERTURE_VIDEO)
        uint8_t pageShift;
        uint8_t pad[3];
    } levels[6];
} nv_copy_pdes_t;

_Static_assert(sizeof(nv_copy_pdes_t) == 184, "NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS");

typedef struct { uint8_t bEnable, bSkipSubmit; } nv_gpfifo_schedule_t;

/* ------------------------------------------- GPU page tables (GMMU v2) */

/* Pascal..Ampere 49-bit VA, 5 levels (nouveau vmmgp100.c):
 *   PD3 VA 48:47 (4 entries), PD2 46:38, PD1 37:29 (512 each, 8 bytes),
 *   PD0 28:21 (256 entries of 16 bytes: big-page PT, small-page PT),
 *   PT  20:12 (512 entries, 4 KiB pages).
 * A PD0 entry can also map a 2 MiB page directly (a PTE in its first
 * 8 bytes). Each table is 4 KiB here (PD3 uses its first 32 bytes). */
#define NV_GMMU_APER_VRAM      0u   // PTE apertures
#define NV_GMMU_APER_SYS_COH   2u
#define NV_GMMU_APER_SYS_NCOH  3u
#define NV_GMMU_PDE_VRAM       1u   // PDE apertures
#define NV_GMMU_PDE_SYS_COH    2u

static inline uint64_t nv_gmmu_pde(uint64_t table_addr, uint32_t aperture) {
    uint64_t d = (table_addr >> 4) | ((uint64_t)aperture << 1);
    if (aperture == NV_GMMU_PDE_SYS_COH) d |= 1ull << 3;     // volatile
    return d;
}

// A valid PTE, pitch kind, read-write.
static inline uint64_t nv_gmmu_pte(uint64_t addr, uint32_t aperture) {
    uint64_t d = (addr >> 4) | 1ull | ((uint64_t)aperture << 1);
    if (aperture == NV_GMMU_APER_SYS_COH || aperture == NV_GMMU_APER_SYS_NCOH) d |= 1ull << 3;
    return d;
}

static inline uint32_t nv_gmmu_index(uint64_t va, int level) {
    // level 4 = PD3 ... 0 = PT
    static const uint8_t shift[5] = { 12, 21, 29, 38, 47 };
    static const uint16_t mask[5] = { 511, 255, 511, 511, 3 };
    return (uint32_t)(va >> shift[level]) & mask[level];
}

/* ------------------------------------------------------ copy engine */

/* TURING_DMA_COPY_A (clc5b5.h). A fill is a remapped copy with no
 * source: every 4-byte element of the destination gets CONST_A. */
#define NVC5B5_SET_SEMAPHORE_A      0x0240u
#define NVC5B5_SET_SEMAPHORE_B      0x0244u
#define NVC5B5_SET_SEMAPHORE_PAYLOAD 0x0248u
#define NVC5B5_LAUNCH_DMA           0x0300u
#define NVC5B5_OFFSET_OUT_UPPER     0x0408u
#define NVC5B5_OFFSET_OUT_LOWER     0x040Cu
#define NVC5B5_PITCH_IN             0x0410u
#define NVC5B5_PITCH_OUT            0x0414u
#define NVC5B5_LINE_LENGTH_IN       0x0418u
#define NVC5B5_LINE_COUNT           0x041Cu
#define NVC5B5_SET_REMAP_CONST_A    0x0700u
#define NVC5B5_SET_REMAP_COMPONENTS 0x0708u

#define NV_CE_SUBCHANNEL 4u

// Pushes a fill of a pitch-linear XRGB8888 (or float32) rectangle at
// GPU VA `dst`, then a one-word semaphore release of `payload` at `sem`.
void nv_push_ce_fill(nv_push_t* p, uint64_t dst, uint32_t width, uint32_t height,
                     uint32_t pitch_bytes, uint32_t value, uint64_t sem, uint32_t payload);

/* Channel memory (nouveau gf100_chan_inst, gv100_chan_userd). */
#define NV_CHAN_INST_SIZE   0x1000u
#define NV_CHAN_RAMFC_SIZE  0x200u
#define NV_CHAN_USERD_SIZE  0x200u
#define NV_USERD_GP_GET     0x88u
#define NV_USERD_GP_PUT     0x8Cu
#define NV_USERMODE_BASE    0x810000u       // BAR0, TURING_USERMODE_A
// Ring the doorbell by writing runlist << 16 | channel id to
// NV_USERMODE_BASE + NV_USERMODE_NOTIFY_CHANNEL_PENDING (nouveau r535/tu102).

/* BAR1 (Turing, nouveau bar/tu102.c). The firmware leaves BAR1 in
 * physical mode: BAR1 offset = VRAM address, which is how the kernel
 * draws the screen and reaches the VRAM heap. GSP-RM switches it to
 * virtual mode with its own page tables while it starts (nouveau then
 * takes those over); we switch it back. */
#define NV_BAR1_BLOCK_TU     0xB80F40u      // bit 31: virtual mode
#define NV_BAR_BIND_STATUS   0xB80F50u      // bits 1:0 BAR1 bind pending/outstanding

/* GspStaticConfigInfo (GET_GSP_STATIC_INFO reply), 535.113.01: size and
 * the offsets used, computed by compiling nouveau's headers. */
#define NV_STATIC_INFO_SIZE            2168u
#define NV_STATIC_INFO_GPU_NAME        1820u
#define NV_STATIC_INFO_INTERNAL_CLIENT 2152u    // then device, subdevice

/* ===================================================================
 * Graphics (nouveau gr/r535.c): context buffers the driver allocates
 * and hands to GSP-RM ("promotes"), a golden context RM records on a
 * throwaway channel when the first 3D object is made, then the 3D
 * channel with its own buffers.
 * =================================================================== */

#define NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO 0x20800A32u
#define NV2080_CTRL_CMD_GPU_PROMOTE_CTX                              0x2080012Bu

typedef struct { uint32_t size, alignment; } nv_ctxbuf_info_t;
typedef struct {                // NV2080_CTRL_INTERNAL_STATIC_GR_GET_CONTEXT_BUFFERS_INFO_PARAMS
    nv_ctxbuf_info_t engine[8][25];   // [GR engine][NV0080 ENGINE_ID]
} nv_ctxbuf_infos_t;
_Static_assert(sizeof(nv_ctxbuf_infos_t) == 1600, "GET_CONTEXT_BUFFERS_INFO_PARAMS");

// NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_*
#define NV_CTX_ENGINE_GRAPHICS          0x00u
#define NV_CTX_ENGINE_PATCH             0x10u
#define NV_CTX_ENGINE_BUNDLE_CB         0x11u
#define NV_CTX_ENGINE_PAGEPOOL_GLOBAL   0x12u
#define NV_CTX_ENGINE_ATTRIBUTE_CB      0x13u
#define NV_CTX_ENGINE_RTV_CB_GLOBAL     0x14u
#define NV_CTX_ENGINE_FECS_EVENT        0x17u
#define NV_CTX_ENGINE_PRIV_ACCESS_MAP   0x18u

// NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_*
#define NV_PROMOTE_MAIN                 0u
#define NV_PROMOTE_PATCH                2u
#define NV_PROMOTE_BUNDLE_CB            3u
#define NV_PROMOTE_PAGEPOOL             4u
#define NV_PROMOTE_ATTRIBUTE_CB         5u
#define NV_PROMOTE_RTV_CB_GLOBAL        6u
#define NV_PROMOTE_FECS_EVENT           9u
#define NV_PROMOTE_PRIV_ACCESS_MAP      10u
#define NV_PROMOTE_UNRESTRICTED_PRIV_ACCESS_MAP 11u

typedef struct {                // NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY
    uint64_t gpuPhysAddr, gpuVirtAddr, size;
    uint32_t physAttr;
    uint16_t bufferId;
    uint8_t bInitialize, bNonmapped;
} nv_promote_entry_t;

typedef struct {                // NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS
    uint32_t engineType;
    uint32_t hClient, ChID, hChanClient, hObject, hVirtMemory;
    uint64_t virtAddress, size;
    uint32_t entryCount;
    uint32_t pad;
    nv_promote_entry_t promoteEntry[16];
} nv_promote_ctx_t;

_Static_assert(sizeof(nv_promote_entry_t) == 32, "NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY");
_Static_assert(offsetof(nv_promote_ctx_t, promoteEntry) == 48, "NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS");
_Static_assert(sizeof(nv_promote_ctx_t) == 560, "NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS");

/* Channel (host) methods, any subchannel (clc46f.h): a semaphore release
 * that first waits for the channel's engine to go idle. */
#define NVC46F_SEM_ADDR_LO      0x005Cu
#define NVC46F_SEM_EXECUTE      0x006Cu     // after ADDR_HI, PAYLOAD_LO/HI
#define NVC46F_SEM_RELEASE_WFI  (1u | (1u << 20))     // RELEASE, WFI, 32-bit payload

void nv_push_host_semaphore(nv_push_t* p, uint64_t sem, uint32_t payload);
