#include "x86_64/nv_rm.h"

/*
 * GSP-RM boot and RPC formats (nv_rm.h). No kernel dependencies: also
 * built for tests/host/nv_hw_tests.c.
 */

static uint32_t rd_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read32(const nv_fw_file_t* f, uint64_t off, uint32_t* out) {
    uint8_t b[4];
    if (off > f->size || f->size - off < 4 || !f->read(f->ctx, off, b, 4)) return false;
    *out = rd_le32(b);
    return true;
}

static bool inside(uint64_t off, uint64_t len, uint64_t size) {
    return off <= size && len <= size - off;
}

bool nv_fw_parse_booter(const nv_fw_file_t* f, nv_booter_t* out, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    nv_fw_bin_hdr_t bin;
    if (!nv_fw_parse_bin(f, &bin, why)) return false;

    uint32_t hs[9];
    for (int i = 0; i < 9; i++)
        if (!read32(f, (uint64_t)bin.header_offset + 4u * i, &hs[i])) { *why = "HS header outside the file"; return false; }
    // sig_prod_offset, sig_prod_size, patch_loc, patch_sig, meta_data_offset,
    // meta_data_size, num_sig, header_offset, header_size
    uint32_t loc, sig, count;
    if (!read32(f, hs[2], &loc) || !read32(f, hs[3], &sig) || !read32(f, hs[6], &count)) {
        *why = "HS patch fields outside the file"; return false;
    }
    if (count == 0 || count > 16 || hs[1] == 0 || hs[1] % count) { *why = "bad signature count"; return false; }

    nv_booter_t b = { 0 };
    b.data_offset = bin.data_offset;
    b.data_size = bin.data_size;
    b.sig_count = count;
    b.sig_size = hs[1] / count;
    b.sig_offset = hs[0] + sig;
    b.patch_loc = loc;
    if (!inside(b.sig_offset, b.sig_size, f->size)) { *why = "signature outside the file"; return false; }
    if (!inside(b.patch_loc, b.sig_size, b.data_size)) { *why = "signature slot outside the ucode"; return false; }

    uint32_t lh[7];
    for (int i = 0; i < 7; i++)
        if (!read32(f, (uint64_t)hs[7] + 4u * i, &lh[i])) { *why = "load header outside the file"; return false; }
    // os_code_offset, os_code_size, os_data_offset, os_data_size, num_apps, app[0].offset, app[0].size
    if (lh[4] < 1) { *why = "no HS application"; return false; }
    b.os_code_offset = lh[0];
    b.os_code_size = lh[1];
    b.os_data_offset = lh[2];
    b.os_data_size = lh[3];
    b.app_offset = lh[5];
    b.app_size = lh[6];
    if (!inside(b.os_code_offset, b.os_code_size, b.data_size) ||
        !inside(b.os_data_offset, b.os_data_size, b.data_size) ||
        !inside(b.app_offset, b.app_size, b.data_size) || b.app_size == 0) {
        *why = "code/data outside the ucode"; return false;
    }
    if ((b.os_code_offset | b.app_offset) & 0xFFu) { *why = "code not 256-byte aligned"; return false; }

    for (int i = 0; i < 3; i++) {
        if (hs[5] < 4u * (i + 1) || !read32(f, (uint64_t)hs[4] + 4u * i, &b.meta[i])) b.meta[i] = 0;
    }
    if (out) *out = b;
    return true;
}

bool nv_fw_parse_rm_bootloader(const nv_fw_file_t* f, nv_rm_bootloader_t* out, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    nv_fw_bin_hdr_t bin;
    if (!nv_fw_parse_bin(f, &bin, why)) return false;
    uint32_t d[19];
    for (int i = 0; i < 19; i++)
        if (!read32(f, (uint64_t)bin.header_offset + 4u * i, &d[i])) { *why = "descriptor outside the file"; return false; }
    nv_rm_bootloader_t r = { 0 };
    r.data_offset = bin.data_offset;
    r.data_size = bin.data_size;
    r.version = d[0];
    r.app_version = d[7];
    r.manifest_offset = d[8];
    r.data_off = d[10];         // monitorDataOffset
    r.code_offset = d[12];      // monitorCodeOffset
    if (r.version < 1 || r.version > 4) { *why = "unknown boot loader descriptor version"; return false; }
    if (out) *out = r;
    return true;
}

static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

uint64_t nv_gsp_vga_workspace(uint32_t reg, uint64_t fb_size) {
    if (reg & 0x8u) {
        uint64_t addr = (uint64_t)(reg >> 8) << 16;
        if (addr < fb_size) return addr;
    }
    return fb_size - 0x20000u;
}

uint64_t nv_vidmem_size(uint32_t reg) {
    uint64_t size = (uint64_t)((reg >> 4) & 0x3Fu) << ((reg & 0xFu) + 20);
    return (reg & 0x40000000u) ? size / 16 * 15 : size;
}

void nv_gsp_layout(nv_gsp_layout_t* l, uint64_t fb_size, uint64_t vga_addr,
                   uint64_t boot_size, uint64_t elf_size) {
    *l = (nv_gsp_layout_t){ 0 };
    l->fb_size = fb_size;
    l->vga_addr = vga_addr;
    l->vga_size = fb_size - vga_addr;
    l->frts_size = 0x100000;
    l->frts_addr = align_down(vga_addr, 0x20000) - l->frts_size;
    l->boot_size = boot_size;
    l->boot_addr = align_down(l->frts_addr - boot_size, 0x1000);
    l->elf_size = elf_size;
    l->elf_addr = align_down(l->boot_addr - elf_size, 0x10000);

    uint64_t gb = (fb_size + (1ull << 30) - 1) >> 30;
    uint64_t heap = NV_GSP_HEAP_BASE + align_up(NV_GSP_HEAP_PER_GB * gb, 1ull << 20) +
                    align_up(NV_GSP_HEAP_CLIENT, 1ull << 20);
    if (heap < NV_GSP_HEAP_MIN) heap = NV_GSP_HEAP_MIN;
    l->heap_addr = align_down(l->elf_addr - heap, 0x100000);
    l->heap_size = align_down(l->elf_addr - l->heap_addr, 0x100000);

    l->wpr2_addr = align_down(l->heap_addr - sizeof(nv_gsp_wpr_meta_t), 0x100000);
    l->wpr2_end = align_down(vga_addr, 0x20000);
    l->nonwpr_size = 0x100000;
    l->nonwpr_addr = l->wpr2_addr - l->nonwpr_size;
}

void nv_gsp_fill_wpr_meta(nv_gsp_wpr_meta_t* m, const nv_gsp_layout_t* l) {
    // The sysmem addresses are filled in by the caller.
    m->magic = NV_GSP_WPR_META_MAGIC;
    m->revision = NV_GSP_WPR_META_REVISION;
    m->sizeOfRadix3Elf = l->elf_size;
    m->sizeOfBootloader = l->boot_size;
    m->gspFwRsvdStart = l->nonwpr_addr;
    m->nonWprHeapOffset = l->nonwpr_addr;
    m->nonWprHeapSize = l->nonwpr_size;
    m->gspFwWprStart = l->wpr2_addr;
    m->gspFwHeapOffset = l->heap_addr;
    m->gspFwHeapSize = l->heap_size;
    m->gspFwOffset = l->elf_addr;
    m->bootBinOffset = l->boot_addr;
    m->frtsOffset = l->frts_addr;
    m->frtsSize = l->frts_size;
    m->gspFwWprEnd = l->wpr2_end;
    m->fbSize = l->fb_size;
    m->vgaWorkspaceOffset = l->vga_addr;
    m->vgaWorkspaceSize = l->vga_size;
    m->bootCount = 0;
    m->verified = 0;
}

uint32_t nv_radix3_pages(uint64_t bytes, int lvl) {
    uint64_t pages = (bytes + NV_GSP_PAGE - 1) / NV_GSP_PAGE;     // image pages
    for (int i = 2; i >= lvl; i--) {
        uint64_t table = (pages * 8 + NV_GSP_PAGE - 1) / NV_GSP_PAGE;
        if (table == 0) table = 1;
        pages = table;
    }
    return (uint32_t)pages;
}

uint64_t nv_libos_id8(const char* name) {
    uint64_t id = 0;
    for (int i = 0; i < 8 && name[i]; i++) id = (id << 8) | (uint8_t)name[i];
    return id;
}

uint32_t nv_gsp_checksum(const void* msg, uint32_t bytes) {
    const uint8_t* p = (const uint8_t*)msg;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < bytes; i += 8) {
        uint64_t w = 0;
        for (uint32_t k = 0; k < 8 && i + k < bytes; k++) w |= (uint64_t)p[i + k] << (8 * k);
        sum ^= w;
    }
    return (uint32_t)sum ^ (uint32_t)(sum >> 32);
}

static void wr_le32(uint8_t* p, uint32_t v) {
    for (int b = 0; b < 4; b++) p[b] = (uint8_t)(v >> (8 * b));
}

uint32_t nv_gsp_pack_registry(uint8_t* out, uint32_t cap, const nv_reg_entry_t* e, uint32_t count) {
    // PACKED_REGISTRY_TABLE { u32 size; u32 numEntries; entries[] }, each
    // entry { u32 nameOffset; u8 type; u32 data; u32 length } (16 bytes),
    // names after the entries. Offsets are from the table start.
    uint32_t at = 8 + 16 * count;
    if (at > cap) return 0;
    for (uint32_t i = 0; i < cap; i++) out[i] = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = 0;
        while (e[i].name[len]) len++;
        if (at + len + 1 > cap) return 0;
        uint8_t* ent = out + 8 + 16 * i;
        wr_le32(ent, at);
        ent[4] = 1;                     // REGISTRY_TABLE_ENTRY_TYPE_DWORD
        wr_le32(ent + 8, e[i].value);
        wr_le32(ent + 12, 4);
        for (uint32_t c = 0; c <= len; c++) out[at + c] = (uint8_t)e[i].name[c];
        at += len + 1;
    }
    wr_le32(out, at);
    wr_le32(out + 4, count);
    return at;
}

int nv_seq_payload_dwords(uint32_t opcode) {
    switch (opcode) {
        case NV_SEQ_REG_WRITE:  return 2;
        case NV_SEQ_REG_MODIFY: return 3;
        case NV_SEQ_REG_POLL:   return 5;
        case NV_SEQ_DELAY_US:   return 1;
        case NV_SEQ_REG_STORE:  return 2;
        case NV_SEQ_CORE_RESET: case NV_SEQ_CORE_START:
        case NV_SEQ_CORE_WAIT_HALT: case NV_SEQ_CORE_RESUME:
            return 0;
        default: return -1;
    }
}

const char* nv_rpc_name(uint32_t function) {
    switch (function) {
        case NV_RPC_GET_GSP_STATIC_INFO:       return "GET_GSP_STATIC_INFO";
        case NV_RPC_GSP_SET_SYSTEM_INFO:       return "GSP_SET_SYSTEM_INFO";
        case NV_RPC_SET_REGISTRY:              return "SET_REGISTRY";
        case NV_RPC_EVENT_GSP_INIT_DONE:       return "GSP_INIT_DONE";
        case NV_RPC_EVENT_RUN_CPU_SEQUENCER:   return "RUN_CPU_SEQUENCER";
        case NV_RPC_EVENT_POST_EVENT:          return "POST_EVENT";
        case NV_RPC_EVENT_RC_TRIGGERED:        return "RC_TRIGGERED";
        case NV_RPC_EVENT_MMU_FAULT_QUEUED:    return "MMU_FAULT_QUEUED";
        case NV_RPC_EVENT_OS_ERROR_LOG:        return "OS_ERROR_LOG";
        case NV_RPC_EVENT_UCODE_LIBOS_PRINT:   return "UCODE_LIBOS_PRINT";
        default:                               return "?";
    }
}

/* ------------------------------------------ FWSEC-FRTS (Turing, v2) */

bool nv_fw_parse_acr_bl(const nv_fw_file_t* f, nv_acr_bl_t* out, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    nv_fw_bin_hdr_t bin;
    if (!nv_fw_parse_bin(f, &bin, why)) return false;
    // nvfw_bl_desc: start_tag, dmem_load_off, code_off, code_size,
    // data_off, data_size
    uint32_t d[6];
    for (int i = 0; i < 6; i++)
        if (!read32(f, (uint64_t)bin.header_offset + 4u * i, &d[i])) {
            *why = "boot loader descriptor outside the file";
            return false;
        }
    nv_acr_bl_t b = { 0 };
    b.start_tag = d[0];
    b.dmem_load_off = d[1];
    b.code_offset = bin.data_offset + d[2];
    b.code_size = d[3];
    if (b.code_size == 0 || b.code_size > 0x10000u || d[2] > bin.data_size ||
        b.code_size > bin.data_size - d[2] || !inside(b.code_offset, b.code_size, f->size)) {
        *why = "boot loader code outside the file";
        return false;
    }
    if (b.start_tag > 0xFFFFu) { *why = "bad boot loader start tag"; return false; }
    *out = b;
    return true;
}

void nv_fwsec_bl_desc(nv_bl_dmem_desc_t* d, const nv_vbios_info_t* v, uint64_t image_pa) {
    for (size_t i = 0; i < sizeof(*d); i++) ((uint8_t*)d)[i] = 0;
    // tu102_gsp_fwsec_load_bld: the non-secure part of IMEM starts at its
    // physical base, the secure part at its own base; DMEM follows in
    // the image at its offset.
    d->ctx_dma = NV_FALCON_DMAIDX_PHYS_SYS_NCOH;
    d->code_dma_base = image_pa;
    d->non_sec_code_off = v->imem_phys;
    d->non_sec_code_size = v->imem_load - v->imem_sec_size;
    d->sec_code_off = v->imem_sec_base;
    d->sec_code_size = v->imem_sec_size;
    d->code_entry_point = 0;
    d->data_dma_base = image_pa + v->dmem_offset;
    d->data_size = v->dmem_load;
}

static void put_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

bool nv_fwsec_patch_frts(uint8_t* image, const nv_vbios_info_t* v, uint64_t frts_addr,
                         uint64_t frts_size, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    if (!v->mapper_offset || v->mapper_offset < v->image_offset) {
        *why = "FWSEC has no DMEM mapper";
        return false;
    }
    if (!(v->mapper_cmd_mask0 & (1u << NV_FWSEC_CMD_FRTS))) {
        *why = "this FWSEC does not offer the FRTS command";
        return false;
    }
    if ((frts_addr | frts_size) & 0xFFFu || (frts_addr >> 12) > 0xFFFFFFFFull || !frts_size) {
        *why = "FRTS region not 4 KiB aligned";
        return false;
    }
    // The mapper (nvfw_falcon_appif_dmemmapper_v3): init_cmd at +44, the
    // command buffer's DMEM offset at +8 and size at +12.
    uint32_t mapper = v->mapper_offset - v->image_offset;
    if ((uint64_t)mapper + 64u > v->stored_size) { *why = "DMEM mapper outside the image"; return false; }
    uint32_t cmd_size = rd_le32(image + mapper + 12);
    uint32_t cmd_in = v->mapper_cmd_in;
    if (cmd_size < NV_FWSEC_FRTS_CMD_BYTES || cmd_in > v->dmem_load ||
        v->dmem_load - cmd_in < NV_FWSEC_FRTS_CMD_BYTES ||
        (uint64_t)v->dmem_offset + cmd_in + NV_FWSEC_FRTS_CMD_BYTES > v->stored_size) {
        *why = "FWSEC's command buffer is too small for FRTS";
        return false;
    }
    put_le32(image + mapper + 44, NV_FWSEC_CMD_FRTS);

    uint8_t* cmd = image + v->dmem_offset + cmd_in;
    for (uint32_t i = 0; i < NV_FWSEC_FRTS_CMD_BYTES; i++) cmd[i] = 0;
    // read_vbios: version 1, 24 bytes, no image (FWSEC reads the ROM
    // itself), flags 2.
    put_le32(cmd + 0, 1);
    put_le32(cmd + 4, 24);
    put_le32(cmd + 20, 2);
    // frts_region: version 1, 20 bytes, address and size in 4 KiB pages, in VRAM.
    put_le32(cmd + 24, 1);
    put_le32(cmd + 28, 20);
    put_le32(cmd + 32, (uint32_t)(frts_addr >> 12));
    put_le32(cmd + 36, (uint32_t)(frts_size >> 12));
    put_le32(cmd + 40, NV_FWSEC_FRTS_REGION_FB);
    *why = "";
    return true;
}

bool nv_wpr2_decode(uint32_t lo_reg, uint32_t hi_reg, uint64_t* lo, uint64_t* hi) {
    if (nv_pri_error(lo_reg) || nv_pri_error(hi_reg)) return false;
    uint64_t l = (uint64_t)(lo_reg >> 4) << 12;
    uint64_t h = (uint64_t)(hi_reg >> 4) << 12;
    if (!h || h <= l) return false;
    if (lo) *lo = l;
    if (hi) *hi = h;
    return true;
}
