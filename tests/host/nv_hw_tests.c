// Host tests for the NVIDIA encoders and firmware parsers (nv_fw.c).
// Build and run: make test-host
#include <stdio.h>
#include <string.h>
#include "x86_64/nv_hw.h"

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
    CHECK(m && n == 4, "TU106 (RTX 2060) has a firmware list");
    CHECK(strcmp(nv_chip_name(0x166), "tu106") == 0, "TU106 name");
    CHECK(m && strcmp(m[3].file, "gsp/gsp-535.113.01.bin") == 0 && m[3].kind == NV_FW_KIND_ELF,
          "GSP-RM image path");
    CHECK(m && strcmp(m[3].sections[1], ".fwsignature_tu10x") == 0, "TU10x signature section");
    m = nv_fw_manifest(0x168, &n);
    CHECK(m && strcmp(m[3].sections[1], ".fwsignature_tu11x") == 0, "TU116 signature section");
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

int main(void) {
    test_push();
    test_bin();
    test_elf();
    test_manifest();
    test_vbios();
    if (failures) {
        printf("nv hw tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("nv hw tests passed\n");
    return 0;
}
