#include <stdarg.h>
#include "x86_64/nv_gpu.h"
#include "x86_64/nv_rm.h"
#include "x86_64/pmm.h"
#include "x86_64/pci.h"
#include "x86_64/minimafs.h"
#include "x86_64/scheduler.h"
#include "string.h"
#include "time.h"

/*
 * Step 2b, first part: FWSEC-FRTS ('gpu nvidia boot').
 *
 * FWSEC is a signed falcon program in the card's VBIOS. Run with the
 * FRTS command it sets up the FRTS region at the top of VRAM and the WPR2
 * protection around it, which booter_load and GSP-RM (nv_gsp_rm.c) need.
 *
 * Turing (VBIOS descriptor v2), as nouveau does it (gsp/fwsec.c,
 * gsp/tu102.c, falcon/gm200.c):
 *   1. wait for the VBIOS's own boot (GFW) to have finished;
 *   2. copy FWSEC's ucode image out of the ROM into sysmem and patch its
 *      DMEM mapper with the FRTS command and the region (nv_rm_fmt.c);
 *   3. reset the GSP falcon, put the ACR boot loader (acr/bl.bin) at the
 *      top of its IMEM and a descriptor at DMEM 0 saying where the image
 *      is; the boot loader DMAs the image in and runs it;
 *   4. wait for the falcon to halt and check FWSEC's error code and WPR2.
 * Ampere and later (descriptor v3) load FWSEC differently (signatures,
 * boot ROM parameters) and are not handled yet.
 */

static const nv_falcon_t k_gsp = { "GSP", NV_FALCON_GSP };

#define FWSEC_HALT_MS 2000u     // nouveau gives the falcon 2 s
#define GFW_BOOT_MS   2000u

static struct {
    gpu_adapter_t* a;
    void (*print)(const char* text);
} s;

static void say(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (s.print) s.print(line);
}

static uint32_t rd(uint32_t reg) { return nv_rd32(s.a, reg); }
static void wr(uint32_t reg, uint32_t v) { nv_wr32(s.a, reg, v); }
static uint32_t frd(uint32_t reg) { return rd(k_gsp.base + reg); }
static void fwr(uint32_t reg, uint32_t v) { wr(k_gsp.base + reg, v); }

/* ------------------------------------------------------------- WPR2 */

bool nv_gsp_wpr2(gpu_adapter_t* a, uint64_t* lo, uint64_t* hi) {
    if (!a || !a->mmio) return false;
    return nv_wpr2_decode(nv_rd32(a, NV_PFB_PRI_MMU_WPR2_ADDR_LO),
                          nv_rd32(a, NV_PFB_PRI_MMU_WPR2_ADDR_HI), lo, hi);
}

/* ------------------------------------------------------------- steps */

// The VBIOS runs its own boot on the falcons at power-on; FWSEC must not
// be started before that has finished.
static bool wait_gfw_boot(void) {
    uint64_t end = time_get_uptime_ms() + GFW_BOOT_MS;
    for (;;) {
        uint32_t mask = rd(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK);
        uint32_t boot = rd(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
        if (!nv_pri_error(mask) && !nv_pri_error(boot) && (mask & 1u) && (boot & 0xFFu) == 0xFFu)
            return true;
        if (time_get_uptime_ms() >= end) {
            say("  the VBIOS has not finished its own boot (GFW_BOOT 0x%x, mask 0x%x)\n", boot, mask);
            return false;
        }
        sleep(1);
    }
}

// Reads the ROM through the PROM window with shadowing off, as the
// report does. Fills `v` and, if `image` is not NULL, FWSEC's ucode.
static bool read_fwsec(nv_vbios_info_t* v, uint8_t** image, uint32_t* pages) {
    uint32_t shadow = rd(NV_PCI_ROM_SHADOW);
    if (nv_pri_error(shadow)) { say("  the ROM shadow register does not answer\n"); return false; }
    wr(NV_PCI_ROM_SHADOW, shadow & ~1u);
    nv_fw_file_t rom = { nv_prom_read, s.a, NV_VBIOS_PROM_SIZE };
    const char* why = "";
    bool ok = nv_vbios_parse(&rom, v, &why);
    if (!ok) say("  VBIOS: %s\n", why);
    if (ok && v->desc_version != 2) {
        say("  FWSEC descriptor v%u (Ampere and later) is not supported yet; only Turing (v2)\n",
            (unsigned)v->desc_version);
        ok = false;
    }
    if (ok) {
        *pages = (v->stored_size + 4095u) / 4096u;
        *image = (uint8_t*)pmm_try_alloc_pages_zeroed(*pages);
        if (!*image) { say("  out of memory for FWSEC's image\n"); ok = false; }
    }
    if (ok && !nv_prom_read(s.a, v->image_offset, *image, v->stored_size)) {
        say("  could not read FWSEC's image from the ROM\n");
        ok = false;
    }
    wr(NV_PCI_ROM_SHADOW, shadow);
    return ok;
}

static bool fs_read(void* ctx, uint64_t offset, void* buf, uint32_t len) {
    minimafs_file_handle_t* h = (minimafs_file_handle_t*)ctx;
    if (offset > 0xFFFFFFFFull || !minimafs_seek(h, (uint32_t)offset)) return false;
    return minimafs_read(h, buf, len) == len;
}

// acr/bl.bin: its code into `code` (one page is plenty: it is about 1 KiB).
static bool load_acr_bl(const char* chip, nv_acr_bl_t* bl, uint8_t* code, uint32_t room) {
    char path[160];
    snprintf(path, sizeof(path), NV_FW_DIR "%s/acr/bl.bin", chip);
    minimafs_file_handle_t* h = minimafs_open(path, true);
    if (!h) { say("  missing %s\n", path); return false; }
    nv_fw_file_t f = { fs_read, h, minimafs_size(h) };
    const char* why = "";
    bool ok = nv_fw_parse_acr_bl(&f, bl, &why);
    if (ok && bl->code_size > room) { why = "boot loader too big"; ok = false; }
    if (ok && !f.read(f.ctx, bl->code_offset, code, bl->code_size)) { why = "read error"; ok = false; }
    minimafs_close(h);
    if (!ok) { say("  acr/bl.bin: %s\n", why); return false; }
    return true;
}

// Engine reset (gp102_flcn_reset_eng), then IMEM/DMEM scrubbing.
static bool gsp_reset(void) {
    fwr(NV_FALCON_ENGINE, frd(NV_FALCON_ENGINE) | 1u);
    (void)frd(NV_FALCON_ENGINE);
    sleep(1);
    fwr(NV_FALCON_ENGINE, frd(NV_FALCON_ENGINE) & ~1u);
    uint64_t end = time_get_uptime_ms() + 100;
    for (;;) {
        uint32_t v = frd(NV_FALCON_DMACTL);
        if (!nv_pri_error(v) && !(v & 0x6u)) return true;
        if (time_get_uptime_ms() >= end) return false;
        sleep(1);
    }
}

static bool wait_halted(uint32_t ms) {
    uint64_t end = time_get_uptime_ms() + ms;
    for (;;) {
        uint32_t c = frd(NV_FALCON_CPUCTL);
        if (!nv_pri_error(c) && (c & NV_FALCON_CPUCTL_HALTED)) return true;
        if (time_get_uptime_ms() >= end) return false;
        sleep(1);
    }
}

/* -------------------------------------------------------------- run */

bool nv_gsp_fwsec_frts(gpu_adapter_t* a, void (*print)(const char* text)) {
    s.a = a;
    s.print = print;
    uint16_t chipset = nv_chipset(a);
    const char* chip = nv_chip_name(chipset);
    if (!chip || chipset < 0x160 || chipset >= 0x170) {
        say("  FWSEC-FRTS is only written for Turing (chipset 0x%x)\n", chipset);
        return false;
    }
    uint64_t lo = 0, hi = 0;
    if (nv_gsp_wpr2(a, &lo, &hi)) {
        say("  WPR2 is already set up (0x%llx - 0x%llx)\n", (unsigned long long)lo,
            (unsigned long long)hi);
        return true;
    }
    nv_falcon_state_t st;
    if (!nv_falcon_read_state(a, &k_gsp, &st)) {
        say("  the GSP falcon does not answer\n");
        return false;
    }
    if (!wait_gfw_boot()) return false;

    // Where FRTS goes: the same layout booter_load is given later.
    uint64_t fb = nv_vidmem_size(rd(NV_PFB_VIDMEM_SIZE));
    uint64_t vga = nv_gsp_vga_workspace(rd(NV_PDISP_VGA_WORKSPACE), fb);
    nv_gsp_layout_t l;
    nv_gsp_layout(&l, fb, vga, 0, 0);
    if (!fb || l.frts_addr >= fb) {
        say("  could not work out the VRAM size (0x%x)\n", rd(NV_PFB_VIDMEM_SIZE));
        return false;
    }
    say("  VRAM %llu MiB, VBIOS workspace 0x%llx, FRTS 0x%llx (+%llu KiB)\n",
        (unsigned long long)(fb >> 20), (unsigned long long)vga, (unsigned long long)l.frts_addr,
        (unsigned long long)(l.frts_size >> 10));

    nv_vbios_info_t v;
    uint8_t* image = NULL;
    uint32_t image_pages = 0;
    uint8_t* bl_code = NULL;
    bool ok = false, still_running = false;
    if (!read_fwsec(&v, &image, &image_pages)) goto done;
    const char* why = "";
    if (!nv_fwsec_patch_frts(image, &v, l.frts_addr, l.frts_size, &why)) {
        say("  FWSEC: %s\n", why);
        goto done;
    }
    say("  FWSEC: %u bytes (IMEM %u, secure %u; DMEM %u), patched with the FRTS command\n",
        (unsigned)v.stored_size, (unsigned)v.imem_load, (unsigned)v.imem_sec_size,
        (unsigned)v.dmem_load);

    nv_acr_bl_t bl;
    bl_code = (uint8_t*)pmm_try_alloc_pages_zeroed(16);
    if (!bl_code) { say("  out of memory\n"); goto done; }
    if (!load_acr_bl(chip, &bl, bl_code, 16u * 4096u)) goto done;
    uint32_t imem = st.imem_bytes;
    uint32_t bl_padded = (bl.code_size + 255u) & ~255u;
    if (!imem || bl_padded > imem || v.imem_load > imem || v.dmem_load > st.dmem_bytes) {
        say("  FWSEC does not fit the GSP falcon (IMEM %u, DMEM %u bytes)\n", (unsigned)imem,
            (unsigned)st.dmem_bytes);
        goto done;
    }

    // Load: reset, allow physical DMA, boot loader at the top of IMEM,
    // sysmem aperture for its DMA index, descriptor at DMEM 0.
    if (!gsp_reset()) { say("  GSP: reset/scrub timeout\n"); goto done; }
    fwr(NV_FALCON_FBIF_CTL, frd(NV_FALCON_FBIF_CTL) | 0x80u);
    fwr(NV_FALCON_DMACTL, 0);
    nv_falcon_imem_write(a, &k_gsp, imem - bl_padded, bl_code, bl.code_size, bl.start_tag, false);
    uint32_t transcfg = NV_FALCON_FBIF_TRANSCFG(NV_FALCON_DMAIDX_PHYS_SYS_NCOH);
    fwr(transcfg, (frd(transcfg) & ~7u) | 5u);
    nv_bl_dmem_desc_t desc;
    nv_fwsec_bl_desc(&desc, &v, (uint64_t)(uintptr_t)image);
    nv_falcon_dmem_write(a, &k_gsp, 0, &desc, sizeof(desc));

    pci_enable_io_busmaster(a->pci);       // the boot loader DMAs from sysmem
    fwr(NV_FALCON_MAILBOX0, 0);
    fwr(NV_FALCON_MAILBOX1, 0);
    say("  Running FWSEC on the GSP falcon ...\n");
    nv_falcon_start(a, &k_gsp, bl.start_tag << 8);
    bool halted = wait_halted(FWSEC_HALT_MS);
    still_running = !halted;
    uint32_t mb0 = frd(NV_FALCON_MAILBOX0), mb1 = frd(NV_FALCON_MAILBOX1);
    uint32_t err = rd(NV_PBUS_SW_SCRATCH_0E) >> 16;
    say("  FWSEC %s, mailbox 0x%x 0x%x, FRTS error code 0x%04x\n",
        halted ? "halted" : "did not halt within 2 s", mb0, mb1, err);
    if (!halted || mb0 != 0 || err) {
        say("  FWSEC-FRTS failed\n");
        goto done;
    }
    if (!nv_gsp_wpr2(a, &lo, &hi)) {
        say("  FWSEC finished, but WPR2 is not set up (0x%x 0x%x)\n",
            rd(NV_PFB_PRI_MMU_WPR2_ADDR_LO), rd(NV_PFB_PRI_MMU_WPR2_ADDR_HI));
        goto done;
    }
    say("  ok: WPR2 0x%llx - 0x%llx\n", (unsigned long long)lo, (unsigned long long)hi);
    if (lo != l.frts_addr)
        say("  note: WPR2 starts at 0x%llx, not at FRTS 0x%llx\n", (unsigned long long)lo,
            (unsigned long long)l.frts_addr);
    ok = true;

done:
    // Once FWSEC has halted (or never started) its image can go back; a
    // falcon still running might yet read it, so then it is kept.
    if (image && !still_running) free_pages(image, image_pages);
    if (bl_code) free_pages(bl_code, 16);
    return ok;
}
