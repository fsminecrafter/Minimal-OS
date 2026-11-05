// Host tests for the NVIDIA encoders and firmware parsers (nv_fw.c).
// Build and run: make test-host
#include <stdio.h>
#include <string.h>
#include "x86_64/nv_hw.h"
#include "x86_64/nv_rm.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

typedef struct {
    const uint8_t* data;
    uint64_t size;
} mem_t;

static bool mem_read(void* ctx, uint64_t offset, void* buf, uint32_t len) {
    mem_t* m = ctx;
    if (offset > m->size || len > m->size - offset) return false;
    memcpy(buf, m->data + offset, len);
    return true;
}

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static void test_push(void) {
    // SET_OBJECT on subchannel 0, one data word.
    CHECK(nv_push_header(NV_PUSH_TYPE_INC, 0, 0x0000, 1) == 0x20010000u, "INC header");
    CHECK(nv_push_header(NV_PUSH_TYPE_INC, 3, 0x19D0, 2) == 0x20026674u, "INC header with subchannel");
    CHECK(nv_push_header(NV_PUSH_TYPE_IMMD, 0, 0x0100, 5) == 0x80050040u, "IMMD header");

    uint32_t words[64];
    nv_push_t p;
    nv_push_init(&p, words, 64);
    nv_push_clear_xrgb(&p, 0x123456000ull, 640, 480, 2560, 0xFF8000);
    CHECK(!p.overflow, "clear fits");
    CHECK(words[0] == 0x20010000u && words[1] == NV_CLASS_TURING_A, "binds TURING_A");
    CHECK(words[2] == nv_push_header(1, 0, 0x800, 8) && words[3] == 0x1 && words[4] == 0x23456000u,
          "render target address");
    CHECK(words[p.count - 2] == nv_push_header(1, 0, NV_3D_CLEAR_BUFFERS, 1), "ends with CLEAR_BUFFERS");
    float r;
    memcpy(&r, &words[p.count - 6], 4);
    CHECK(r == 1.0f, "clear red = 1.0");

    nv_push_init(&p, words, 3);
    uint32_t data[4] = { 1, 2, 3, 4 };
    nv_push_method(&p, 0, 0x100, data, 4);
    CHECK(p.overflow && p.count == 0, "overflow is reported, nothing written");
    nv_push_init(&p, words, 4);
    nv_push_immd(&p, 0, 0x100, 0x2000);
    CHECK(p.overflow, "IMMD data over 13 bits rejected");

    uint32_t e[2];
    nv_gpfifo_entry(0x12345678900ull, 256, e);
    CHECK(e[0] == 0x45678900u && e[1] == (0x123u | (256u << 10)), "GPFIFO entry");
}

static void test_bin(void) {
    uint8_t f[128];
    memset(f, 0, sizeof f);
    put32(f, NV_FW_BIN_MAGIC);
    put32(f + 4, 1);
    put32(f + 8, sizeof f);
    put32(f + 12, 24);          // descriptor
    put32(f + 16, 64);          // ucode
    put32(f + 20, 64);
    mem_t m = { f, sizeof f };
    nv_fw_file_t file = { mem_read, &m, sizeof f };
    nv_fw_bin_hdr_t h;
    const char* why = NULL;
    CHECK(nv_fw_parse_bin(&file, &h, &why), "valid .bin parses");
    CHECK(h.data_offset == 64 && h.data_size == 64, "fields");

    put32(f + 20, 65);
    CHECK(!nv_fw_parse_bin(&file, &h, &why), "ucode past the end rejected");
    put32(f + 20, 64);
    put32(f + 8, 4096);
    CHECK(nv_fw_parse_bin(&file, &h, &why),
          "bin_size past the file accepted (NVIDIA's real files do this)");
    put32(f + 16, 120);
    put32(f + 20, 16);
    CHECK(!nv_fw_parse_bin(&file, &h, &why) && strstr(why, "ucode"),
          "ucode past the real file end rejected");
    put32(f + 16, 64);
    put32(f + 20, 64);
    put32(f + 8, sizeof f);
    put32(f, 0x1234);
    CHECK(!nv_fw_parse_bin(&file, &h, &why) && strstr(why, "magic"), "bad magic rejected");
    file.size = 10;
    CHECK(!nv_fw_parse_bin(&file, &h, &why), "short file rejected");
}

static void test_elf(void) {
    // ELF header, then section names, then 3 section headers:
    // [0] null, [1] .shstrtab, [2] .fwimage
    static uint8_t f[512];
    memset(f, 0, sizeof f);
    memcpy(f, "\x7f" "ELF", 4);
    f[4] = 2; f[5] = 1; f[6] = 1;
    const char names[] = "\0.shstrtab\0.fwimage\0";
    memcpy(f + 64, names, sizeof names);
    uint64_t shoff = 128;
    put64(f + 40, shoff);
    put16(f + 58, 64);
    put16(f + 60, 3);
    put16(f + 62, 1);
    uint8_t* s1 = f + shoff + 64;
    put32(s1, 1);
    put64(s1 + 24, 64);
    put64(s1 + 32, sizeof names);
    uint8_t* s2 = f + shoff + 128;
    put32(s2, 11);
    put64(s2 + 24, 400);
    put64(s2 + 32, 100);
    mem_t m = { f, sizeof f };
    nv_fw_file_t file = { mem_read, &m, sizeof f };
    uint64_t off = 0, size = 0;
    const char* why = NULL;
    CHECK(nv_fw_elf_section(&file, ".fwimage", &off, &size, &why) && off == 400 && size == 100,
          "finds .fwimage");
    CHECK(!nv_fw_elf_section(&file, ".fwsignature_tu10x", &off, &size, &why) &&
          strcmp(why, "section missing") == 0, "missing section reported");
    CHECK(!nv_fw_elf_section(&file, ".fwimag", &off, &size, &why), "prefix does not match");
    put64(s2 + 32, 200);
    CHECK(!nv_fw_elf_section(&file, ".fwimage", &off, &size, &why), "section past the end rejected");
    put16(f + 62, 7);
    CHECK(!nv_fw_elf_section(&file, ".fwimage", &off, &size, &why), "bad shstrndx rejected");
    f[4] = 1;
    CHECK(!nv_fw_elf_section(&file, ".fwimage", &off, &size, &why), "32-bit ELF rejected");
}

static void test_manifest(void) {
    uint32_t n = 0;
    const nv_fw_entry_t* m = nv_fw_manifest(0x166, &n);
    CHECK(m && n == 5, "TU106 (RTX 2060) has a firmware list");
    CHECK(strcmp(nv_chip_name(0x166), "tu106") == 0, "TU106 name");
    CHECK(m && strcmp(m[4].file, "gsp/gsp-535.113.01.bin") == 0 && m[4].kind == NV_FW_KIND_ELF,
          "GSP-RM image path");
    CHECK(m && strcmp(m[4].sections[1], ".fwsignature_tu10x") == 0, "TU10x signature section");
    m = nv_fw_manifest(0x168, &n);
    CHECK(m && strcmp(m[4].sections[1], ".fwsignature_tu11x") == 0, "TU116 signature section");
    CHECK(!nv_fw_manifest(0x134, &n) && n == 0, "Pascal has no list yet");
    CHECK(nv_pri_error(0xBADF1100u) && nv_pri_error(0xFFFFFFFFu) && !nv_pri_error(0x16600A1u),
          "PRI error values");
    // HWCFG 0x00020100: IMEM 0x100 * 256 = 64 KiB, DMEM 0x100 * 256 = 64 KiB
    CHECK(nv_falcon_imem_bytes(0x00020100u) == 64 * 1024 && nv_falcon_dmem_bytes(0x00020100u) == 64 * 1024,
          "falcon memory sizes from HWCFG");
}

// One PCI ROM image at `base`: signature, PCIR (and an NPDE when `npde`).
static void put_image(uint8_t* rom, uint32_t base, uint32_t blocks, uint8_t type, int last, int npde) {
    put16(rom + base, 0xAA55);
    put16(rom + base + 0x18, 0x40);
    uint8_t* pcir = rom + base + 0x40;
    memcpy(pcir, "PCIR", 4);
    put16(pcir + 0x0A, 0x18);
    put16(pcir + 0x10, npde ? 1 : (uint16_t)blocks);   // the NPDE overrides this
    pcir[0x14] = type;
    pcir[0x15] = last && !npde ? 0x80 : 0;
    if (npde) {
        uint8_t* e = rom + base + 0x60;                  // (0x40 + 0x18 + 15) & ~15
        memcpy(e, "NPDE", 4);
        put16(e + 0x08, (uint16_t)blocks);
        e[0x0A] = last ? 0x80 : 0;
    }
}

static void test_vbios(void) {
    static uint8_t rom[0x4000];
    memset(rom, 0, sizeof(rom));
    // image 0 (x86, 1 KiB) with the BIT table, an EFI image, then the
    // extended image at 0x3400 holding the PMU table and FWSEC.
    put_image(rom, 0x0000, 2, 0x00, 0, 1);
    put_image(rom, 0x0400, 24, 0x03, 0, 0);
    put_image(rom, 0x3400, 6, 0xE0, 1, 0);
    uint8_t* bit = rom + 0x100;
    bit[0] = 0xFF; bit[1] = 0xB8; memcpy(bit + 2, "BIT", 4);
    bit[8] = 12; bit[9] = 6; bit[10] = 2;
    bit[12] = 'i';                                       // another token first
    bit[18] = 'p'; bit[19] = 2; put16(bit + 20, 4); put16(bit + 22, 0x200);
    // 'p' data: a pointer past image 0 -> 0x3400 + (0x0480 - 0x400).
    put32(rom + 0x200, 0x0480);
    uint8_t* pmu = rom + 0x3480;
    pmu[0] = 1; pmu[1] = 4; pmu[2] = 6; pmu[3] = 2;
    pmu[4] = 0x45; put32(pmu + 6, 0x0500);               // another ucode
    pmu[10] = 0x85; put32(pmu + 12, 0x0500);             // FWSEC at 0x3500
    uint8_t* d = rom + 0x3500;
    put32(d, (uint32_t)(44 + 384) << 16 | 3u << 8 | 1u);
    put32(d + 4, 0x300);                                 // stored size
    put32(d + 12, 0x10);                                 // interface offset in DMEM
    put32(d + 20, 0x200);                                // IMEM load size
    put32(d + 32, 0x100);                                // DMEM load size
    d[39] = 1;                                           // one signature
    uint8_t* dmem = d + 44 + 384 + 0x200;
    uint8_t* appif = dmem + 0x10;
    appif[0] = 1; appif[1] = 4; appif[2] = 8; appif[3] = 2;
    put32(appif + 4, 0x1); put32(appif + 8, 0x40);
    put32(appif + 12, NV_FALCON_APPIF_DMEMMAPPER); put32(appif + 16, 0x80);
    put32(dmem + 0x80, NV_FWSEC_DMAP_SIGNATURE);
    put32(dmem + 0x80 + 8, 0xC0);
    put32(dmem + 0x80 + 52, 1u << NV_FWSEC_CMD_FRTS);

    mem_t m = { rom, sizeof(rom) };
    nv_fw_file_t f = { mem_read, &m, sizeof(rom) };
    nv_vbios_info_t info;
    const char* why = "";
    bool ok = nv_vbios_parse(&f, &info, &why);
    if (!ok) printf("  vbios: %s\n", why);
    CHECK(ok, "vbios parses");
    CHECK(info.image_count == 3 && info.images[0].size == 1024 && info.images[1].type == 3,
          "image chain, NPDE size");
    CHECK(info.image0_size == 1024 && info.extended_base == 0x3400, "extended image");
    CHECK(info.bit_offset == 0x100, "BIT found");
    CHECK(info.pmu_table == 0x3480 && info.pmu_entries == 2, "PMU table through the extended image");
    CHECK(info.fwsec_desc == 0x3500 && info.desc_version == 3 && info.signature_count == 1,
          "FWSEC descriptor");
    CHECK(info.image_offset == 0x3500 + 44 + 384, "ucode image after the signatures");
    CHECK(info.mapper_offset == (uint32_t)(dmem + 0x80 - rom) && info.mapper_cmd_in == 0xC0,
          "DMEM mapper");

    // Broken mapper signature and a missing FWSEC entry are reported.
    put32(dmem + 0x80, 0);
    CHECK(!nv_vbios_parse(&f, &info, &why) && info.fwsec_desc == 0x3500, "bad DMAP rejected");
    pmu[10] = 0x86;
    CHECK(!nv_vbios_parse(&f, &info, &why) && !info.fwsec_desc, "no FWSEC entry");
    memset(rom, 0, 2);
    CHECK(!nv_vbios_parse(&f, &info, &why) && !info.image_count, "no ROM signature");
}

// Header values of linux-firmware nvidia/tu102/gsp/booter_load-535.113.01.bin.
static void test_booter(void) {
    static uint8_t f[0x88 + 0xE200];
    memset(f, 0, sizeof f);
    uint32_t bin[6] = { 0x10DE, 1, 0xE300, 0x18, 0x88, 0xE200 };
    for (int i = 0; i < 6; i++) put32(f + 4 * i, bin[i]);
    uint32_t hs[9] = { 0x3C, 0x10, 0x4C, 0x50, 0x54, 0xC, 0x60, 0x64, 0x24 };
    for (int i = 0; i < 9; i++) put32(f + 0x18 + 4 * i, hs[i]);
    put32(f + 0x4C, 0x8200);                    // patch location
    put32(f + 0x50, 0);                         // signature index offset
    put32(f + 0x54, 0); put32(f + 0x58, 1); put32(f + 0x5C, 0xD);   // fuse, engine, ucode id
    put32(f + 0x60, 1);                         // one signature
    uint32_t lh[7] = { 0, 0x100, 0x8000, 0x6200, 1, 0x100, 0x7F00 };
    for (int i = 0; i < 7; i++) put32(f + 0x64 + 4 * i, lh[i]);
    mem_t m = { f, sizeof f };
    nv_fw_file_t file = { mem_read, &m, sizeof f };
    nv_booter_t b;
    const char* why = "";
    CHECK(nv_fw_parse_booter(&file, &b, &why), "booter_load parses");
    CHECK(b.data_offset == 0x88 && b.data_size == 0xE200, "booter ucode");
    CHECK(b.sig_offset == 0x3C && b.sig_size == 16 && b.sig_count == 1, "booter signature");
    CHECK(b.patch_loc == 0x8200, "booter signature slot");
    CHECK(b.os_code_size == 0x100 && b.app_offset == 0x100 && b.app_size == 0x7F00, "booter code");
    CHECK(b.os_data_offset == 0x8000 && b.os_data_size == 0x6200, "booter data");
    CHECK(b.meta[2] == 0xD, "booter ucode id");
    put32(f + 0x4C, 0xE1F8);
    CHECK(!nv_fw_parse_booter(&file, &b, &why), "signature slot past the ucode rejected");
}

// nvidia/tu102/gsp/bootloader-535.113.01.bin: descriptor v4, offsets 0.
static void test_rm_bootloader(void) {
    static uint8_t f[0x64 + 0x1000];
    memset(f, 0, sizeof f);
    uint32_t bin[6] = { 0x10DE, 1, 0x1100, 0x18, 0x64, 0x1000 };
    for (int i = 0; i < 6; i++) put32(f + 4 * i, bin[i]);
    uint32_t d[5] = { 4, 0, 0x488, 0x488, 0x10 };
    for (int i = 0; i < 5; i++) put32(f + 0x18 + 4 * i, d[i]);
    mem_t m = { f, sizeof f };
    nv_fw_file_t file = { mem_read, &m, sizeof f };
    nv_rm_bootloader_t r;
    const char* why = "";
    CHECK(nv_fw_parse_rm_bootloader(&file, &r, &why), "boot loader parses");
    CHECK(r.version == 4 && r.data_offset == 0x64 && r.data_size == 0x1000, "boot loader image");
    CHECK(r.code_offset == 0 && r.data_off == 0 && r.manifest_offset == 0, "boot loader offsets");
}

static void test_gsp_layout(void) {
    // The RTX 2060 in the bring-up photos: 6 GiB, VGA workspace at
    // 0x17FF00000, FWSEC-FRTS put FRTS at 0x17FE00000.
    CHECK(nv_vidmem_size(0x60u | 0xAu) == 6ull << 30, "VRAM size register");
    uint64_t fb = 6ull << 30;
    uint64_t vga = nv_gsp_vga_workspace(((uint32_t)(0x17FF00000ull >> 16) << 8) | 0x8u, fb);
    CHECK(vga == 0x17FF00000ull, "VGA workspace from the display register");
    CHECK(nv_gsp_vga_workspace(0, fb) == fb - 0x20000, "VGA workspace fallback");

    nv_gsp_layout_t l;
    nv_gsp_layout(&l, fb, vga, 0x1000, 0x1600000);
    CHECK(l.frts_addr == 0x17FE00000ull && l.frts_size == 0x100000, "FRTS where FWSEC put it");
    CHECK(l.boot_addr == 0x17FDFF000ull, "boot loader below FRTS");
    CHECK(l.elf_addr == 0x17E7F0000ull, "GSP-RM image below the boot loader, 64 KiB aligned");
    CHECK(l.heap_size == 105ull << 20 && l.heap_addr == 0x177E00000ull, "heap: 8 + 1 + 96 MiB");
    CHECK(l.wpr2_addr < l.heap_addr && l.heap_addr - l.wpr2_addr <= 0x100000, "WPR2 starts below the heap");
    CHECK(l.wpr2_end == 0x17FF00000ull && l.nonwpr_addr == l.wpr2_addr - 0x100000, "WPR2 end, non-WPR heap");

    nv_gsp_wpr_meta_t m;
    memset(&m, 0, sizeof m);
    nv_gsp_fill_wpr_meta(&m, &l);
    CHECK(m.magic == NV_GSP_WPR_META_MAGIC && m.frtsOffset == l.frts_addr && m.fbSize == fb, "WPR meta");

    CHECK(nv_radix3_pages(0x1600000, 2) == 11 && nv_radix3_pages(0x1600000, 1) == 1 &&
          nv_radix3_pages(0x1600000, 0) == 1, "radix3 table sizes");
}

static void test_rpc(void) {
    CHECK(nv_libos_id8("LOGINIT") == 0x4C4F47494E4954ull, "LibOS id8");
    CHECK(nv_libos_id8("RMARGS") == 0x524D41524753ull, "LibOS id8, short");

    uint8_t msg[24] = { 0 };
    put64(msg, 0x1111111122222222ull);
    put64(msg + 8, 0x0000000100000002ull);
    put32(msg + 16, 0x5);
    CHECK(nv_gsp_checksum(msg, 20) == ((0x11111111u ^ 0x22222222u) ^ (1u ^ 2u) ^ 5u), "checksum");
    CHECK(nv_gsp_msg_slots(48 + 32) == 1 && nv_gsp_msg_slots(0x1001) == 2, "queue slots");

    uint8_t reg[64];
    nv_reg_entry_t e[1] = { { "RMSecBusResetEnable", 1 } };
    uint32_t n = nv_gsp_pack_registry(reg, sizeof reg, e, 1);
    CHECK(n == 24 + 20 && reg[0] == 44 && reg[4] == 1, "registry table header");
    CHECK(reg[8] == 24 && reg[12] == 1 && reg[16] == 1 && reg[20] == 4, "registry entry");
    CHECK(memcmp(reg + 24, "RMSecBusResetEnable", 20) == 0, "registry name");
    CHECK(nv_gsp_pack_registry(reg, 30, e, 1) == 0, "registry overflow");

    CHECK(nv_seq_payload_dwords(NV_SEQ_REG_POLL) == 5 && nv_seq_payload_dwords(9) == -1, "sequencer sizes");
}

// FWSEC-FRTS on Turing: the ACR boot loader, its DMEM descriptor, the
// FRTS patch and WPR2.
static void test_fwsec_frts(void) {
    // acr/bl.bin: bin header, nvfw_bl_desc at 0x18, code at 0x100.
    static uint8_t f[0x600];
    memset(f, 0, sizeof f);
    uint32_t bin[6] = { 0x10DE, 1, sizeof f, 0x18, 0x100, 0x500 };
    for (int i = 0; i < 6; i++) put32(f + 4 * i, bin[i]);
    uint32_t bld[6] = { 0xFD, 0, 0x0, 0x400, 0x400, 0x100 };
    for (int i = 0; i < 6; i++) put32(f + 0x18 + 4 * i, bld[i]);
    mem_t m = { f, sizeof f };
    nv_fw_file_t file = { mem_read, &m, sizeof f };
    nv_acr_bl_t bl;
    const char* why = "";
    CHECK(nv_fw_parse_acr_bl(&file, &bl, &why), "acr/bl.bin parses");
    CHECK(bl.start_tag == 0xFD && bl.code_offset == 0x100 && bl.code_size == 0x400,
          "boot loader code and tag");
    put32(f + 0x18 + 12, 0x600);
    CHECK(!nv_fw_parse_acr_bl(&file, &bl, &why), "boot loader past its data rejected");

    // A v2 FWSEC image: IMEM 0x400 (secure 0x100 at 0x300), DMEM 0x200 at
    // image offset 0x400, mapper at DMEM 0x40, command buffer at DMEM 0x100.
    static uint8_t img[0x600];
    memset(img, 0xEE, sizeof img);
    nv_vbios_info_t v;
    memset(&v, 0, sizeof v);
    v.desc_version = 2;
    v.image_offset = 0x9000;
    v.stored_size = sizeof img;
    v.imem_phys = 0; v.imem_load = 0x400; v.imem_sec_base = 0x300; v.imem_sec_size = 0x100;
    v.dmem_offset = 0x400; v.dmem_load = 0x200;
    v.mapper_offset = v.image_offset + 0x400 + 0x40;
    v.mapper_cmd_in = 0x100;
    v.mapper_cmd_mask0 = 0x00044000u;                       // RTX 2060 TU106 value
    put32(img + 0x440 + 12, 0x100);                         // command buffer size
    CHECK(nv_fwsec_patch_frts(img, &v, 0x3FD00000ull, 0x100000, &why), "FRTS patch");
    uint8_t* cmd = img + 0x400 + 0x100;
    CHECK(img[0x440 + 44] == NV_FWSEC_CMD_FRTS && img[0x440 + 45] == 0, "mapper init command");
    CHECK(cmd[0] == 1 && cmd[4] == 24 && cmd[8] == 0 && cmd[16] == 0 && cmd[20] == 2,
          "read_vbios arguments");
    CHECK(cmd[24] == 1 && cmd[28] == 20 && cmd[40] == NV_FWSEC_FRTS_REGION_FB, "frts_region header");
    CHECK(cmd[32] == 0x00 && cmd[33] == 0xFD && cmd[34] == 0x03 && cmd[36] == 0x00 && cmd[37] == 0x01,
          "FRTS region in 4 KiB pages");
    CHECK(cmd[44] == 0xEE, "nothing written past the command");

    put32(img + 0x440 + 12, 40);
    CHECK(!nv_fwsec_patch_frts(img, &v, 0x3FD00000ull, 0x100000, &why), "small command buffer rejected");
    put32(img + 0x440 + 12, 0x100);
    // The command mask is informational only (see nv_fwsec_patch_frts):
    // the RTX 2060's own value must not stop the patch.
    v.mapper_cmd_mask0 = 0x00044000u;
    CHECK(!nv_fwsec_patch_frts(img, &v, 0x3FD00800ull, 0x100000, &why), "unaligned region rejected");

    nv_bl_dmem_desc_t d;
    nv_fwsec_bl_desc(&d, &v, 0x12345000ull);
    const uint8_t* raw = (const uint8_t*)&d;
    CHECK(d.ctx_dma == NV_FALCON_DMAIDX_PHYS_SYS_NCOH && raw[32] == 4, "descriptor DMA index at 32");
    CHECK(d.code_dma_base == 0x12345000ull && raw[36] == 0x00 && raw[37] == 0x50, "code base at 36");
    CHECK(d.non_sec_code_off == 0 && d.non_sec_code_size == 0x300, "non-secure code");
    CHECK(d.sec_code_off == 0x300 && d.sec_code_size == 0x100, "secure code");
    CHECK(d.data_dma_base == 0x12345400ull && raw[64] == 0x00 && raw[65] == 0x54, "data base at 64");
    CHECK(d.data_size == 0x200 && raw[72] == 0x00 && raw[73] == 0x02, "data size at 72");

    uint64_t lo = 0, hi = 0;
    CHECK(nv_wpr2_decode(0x003FD000u, 0x003FE000u, &lo, &hi) && lo == 0x3FD00000ull && hi == 0x3FE00000ull,
          "WPR2 decoded");
    CHECK(!nv_wpr2_decode(0, 0, &lo, &hi), "no WPR2");
    CHECK(!nv_wpr2_decode(0xBADF1100u, 0x003FE000u, &lo, &hi), "PRI error is no WPR2");
}

int main(void) {
    test_booter();
    test_rm_bootloader();
    test_gsp_layout();
    test_rpc();
    test_push();
    test_bin();
    test_elf();
    test_manifest();
    test_vbios();
    test_fwsec_frts();
    if (failures) {
        printf("nv hw tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("nv hw tests passed\n");
    return 0;
}
