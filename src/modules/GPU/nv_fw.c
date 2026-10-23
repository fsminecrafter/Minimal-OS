#include "x86_64/nv_hw.h"
#include <stddef.h>

/*
 * NVIDIA pushbuffer encoders and firmware file parsers (nv_hw.h).
 * No kernel dependencies: also built for tests/host/nv_hw_tests.c.
 */

/* --------------------------------------------------------- pushbuffers */

void nv_push_init(nv_push_t* p, uint32_t* words, uint32_t capacity) {
    p->words = words;
    p->capacity = capacity;
    p->count = 0;
    p->overflow = false;
}

void nv_push_method(nv_push_t* p, uint32_t subc, uint32_t method, const uint32_t* data, uint32_t count) {
    if (count == 0 || count > 0x1FFFu || p->overflow || p->capacity - p->count < 1 + count) {
        p->overflow = true;
        return;
    }
    p->words[p->count++] = nv_push_header(NV_PUSH_TYPE_INC, subc, method, count);
    for (uint32_t i = 0; i < count; i++) p->words[p->count++] = data[i];
}

void nv_push_immd(nv_push_t* p, uint32_t subc, uint32_t method, uint32_t value) {
    if (value > 0x1FFFu || p->overflow || p->count >= p->capacity) {
        p->overflow = true;
        return;
    }
    p->words[p->count++] = nv_push_header(NV_PUSH_TYPE_IMMD, subc, method, value);
}

void nv_gpfifo_entry(uint64_t gpu_va, uint32_t words, uint32_t out[2]) {
    out[0] = (uint32_t)gpu_va & ~3u;
    out[1] = (uint32_t)(gpu_va >> 32) | (words << 10);
}

static uint32_t float_bits(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

void nv_push_clear_xrgb(nv_push_t* p, uint64_t gpu_va, uint32_t width, uint32_t height,
                        uint32_t pitch_bytes, uint32_t xrgb) {
    nv_push_1(p, 0, NV_3D_SET_OBJECT, NV_CLASS_TURING_A);
    // Render target 0: address, size, format, linear (not block-linear),
    // one layer.
    uint32_t rt[8] = {
        (uint32_t)(gpu_va >> 32), (uint32_t)gpu_va, pitch_bytes, height,
        NV_RT_FORMAT_BGRX8_UNORM, NV_3D_RT_TILE_MODE_LINEAR, 1, 0,
    };
    nv_push_method(p, 0, NV_3D_RT_ADDRESS_HIGH(0), rt, 8);
    nv_push_1(p, 0, NV_3D_RT_CONTROL, (076543210u << 4) | 1u);   // one target, identity map
    uint32_t scissor[2] = { width << 16, height << 16 };
    nv_push_method(p, 0, NV_3D_SCREEN_SCISSOR_HORIZ, scissor, 2);
    const float k = 1.0f / 255.0f;
    uint32_t color[4] = {
        float_bits((float)((xrgb >> 16) & 255u) * k), float_bits((float)((xrgb >> 8) & 255u) * k),
        float_bits((float)(xrgb & 255u) * k), float_bits(1.0f),
    };
    nv_push_method(p, 0, NV_3D_CLEAR_COLOR(0), color, 4);
    nv_push_1(p, 0, NV_3D_CLEAR_BUFFERS, NV_3D_CLEAR_BUFFERS_RGBA | (0u << 6));
}

/* ------------------------------------------------------------ firmware */

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

static bool range_ok(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}

bool nv_fw_parse_bin(const nv_fw_file_t* f, nv_fw_bin_hdr_t* out, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    uint8_t b[24];
    if (f->size < sizeof(b) || !f->read(f->ctx, 0, b, sizeof(b))) { *why = "too short"; return false; }
    nv_fw_bin_hdr_t h = {
        le32(b), le32(b + 4), le32(b + 8), le32(b + 12), le32(b + 16), le32(b + 20),
    };
    if (h.bin_magic != NV_FW_BIN_MAGIC) { *why = "not an NVIDIA firmware file (bad magic)"; return false; }
    // bin_size is not the file length: NVIDIA's published files say more
    // (booter_load-535.113.01: 0xE300, file 0xE288, which ends exactly
    // where the ucode ends). nouveau ignores it too; check the real size.
    if (h.header_offset < sizeof(b) || !range_ok(h.header_offset, 4, f->size)) {
        *why = "descriptor outside the file"; return false;
    }
    if (h.data_size == 0 || !range_ok(h.data_offset, h.data_size, f->size)) {
        *why = "ucode outside the file"; return false;
    }
    if (out) *out = h;
    return true;
}

bool nv_fw_elf_section(const nv_fw_file_t* f, const char* name, uint64_t* offset, uint64_t* size,
                       const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    uint8_t eh[64];
    if (f->size < sizeof(eh) || !f->read(f->ctx, 0, eh, sizeof(eh))) { *why = "too short"; return false; }
    if (eh[0] != 0x7F || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F' || eh[4] != 2 || eh[5] != 1) {
        *why = "not a 64-bit little-endian ELF file"; return false;
    }
    uint64_t shoff = le64(eh + 40);
    uint16_t shentsize = le16(eh + 58), shnum = le16(eh + 60), shstrndx = le16(eh + 62);
    if (shentsize < 64 || shnum == 0 || shstrndx >= shnum ||
        !range_ok(shoff, (uint64_t)shentsize * shnum, f->size)) {
        *why = "bad section table"; return false;
    }
    uint8_t sh[64];
    if (!f->read(f->ctx, shoff + (uint64_t)shstrndx * shentsize, sh, sizeof(sh))) {
        *why = "read error"; return false;
    }
    uint64_t str_off = le64(sh + 24), str_size = le64(sh + 32);
    if (!range_ok(str_off, str_size, f->size)) { *why = "bad section name table"; return false; }
    uint32_t want = 0;
    while (name[want]) want++;
    for (uint16_t i = 0; i < shnum; i++) {
        if (!f->read(f->ctx, shoff + (uint64_t)i * shentsize, sh, sizeof(sh))) {
            *why = "read error"; return false;
        }
        uint32_t name_off = le32(sh);
        if (name_off >= str_size || str_size - name_off < want + 1) continue;
        char got[64];
        if (want + 1 > sizeof(got) || !f->read(f->ctx, str_off + name_off, got, want + 1)) continue;
        bool match = got[want] == 0;
        for (uint32_t k = 0; match && k < want; k++) match = got[k] == name[k];
        if (!match) continue;
        uint64_t off = le64(sh + 24), len = le64(sh + 32);
        if (!range_ok(off, len, f->size)) { *why = "section outside the file"; return false; }
        if (offset) *offset = off;
        if (size) *size = len;
        return true;
    }
    *why = "section missing";
    return false;
}

/* ----------------------------------------------------- firmware lists */

#define GSP_FILE(name) "gsp/" name "-" NV_GSP_FW_VERSION ".bin"

// Turing boots GSP-RM: booter_load runs on SEC2 (heavy-secure), checks
// and starts the GSP-RM image on the GSP core through its boot loader.
// The images are signed per chip; the signature section differs between
// TU10x and TU11x.
static const nv_fw_entry_t k_tu10x[] = {
    { GSP_FILE("booter_load"), NV_FW_KIND_BIN, "SEC2 program that starts GSP-RM", { 0, 0 } },
    { GSP_FILE("booter_unload"), NV_FW_KIND_BIN, "SEC2 program that stops GSP-RM", { 0, 0 } },
    { GSP_FILE("bootloader"), NV_FW_KIND_BIN, "GSP RISC-V boot loader", { 0, 0 } },
    { GSP_FILE("gsp"), NV_FW_KIND_ELF, "GSP-RM (the GPU's firmware driver)",
      { ".fwimage", ".fwsignature_tu10x" } },
};

static const nv_fw_entry_t k_tu11x[] = {
    { GSP_FILE("booter_load"), NV_FW_KIND_BIN, "SEC2 program that starts GSP-RM", { 0, 0 } },
    { GSP_FILE("booter_unload"), NV_FW_KIND_BIN, "SEC2 program that stops GSP-RM", { 0, 0 } },
    { GSP_FILE("bootloader"), NV_FW_KIND_BIN, "GSP RISC-V boot loader", { 0, 0 } },
    { GSP_FILE("gsp"), NV_FW_KIND_ELF, "GSP-RM (the GPU's firmware driver)",
      { ".fwimage", ".fwsignature_tu11x" } },
};

const char* nv_chip_name(uint16_t chipset) {
    switch (chipset) {
        case 0x162: return "tu102";
        case 0x164: return "tu104";
        case 0x166: return "tu106";
        case 0x167: return "tu117";
        case 0x168: return "tu116";
        default:    return NULL;
    }
}

const nv_fw_entry_t* nv_fw_manifest(uint16_t chipset, uint32_t* count) {
    switch (chipset) {
        case 0x162: case 0x164: case 0x166:
            *count = sizeof(k_tu10x) / sizeof(k_tu10x[0]);
            return k_tu10x;
        case 0x167: case 0x168:
            *count = sizeof(k_tu11x) / sizeof(k_tu11x[0]);
            return k_tu11x;
        default:
            *count = 0;
            return NULL;
    }
}
