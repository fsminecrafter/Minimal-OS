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
    CHECK(!nv_fw_parse_bin(&file, &h, &why), "bin_size past the file rejected");
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

int main(void) {
    test_push();
    test_bin();
    test_elf();
    test_manifest();
    if (failures) {
        printf("nv hw tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("nv hw tests passed\n");
    return 0;
}
