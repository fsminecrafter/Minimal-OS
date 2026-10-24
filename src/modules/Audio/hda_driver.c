/*
 * Intel High Definition Audio (HDA) driver
 *
 * Output only, polled (no IRQs), the same model as ac97_driver.c:
 * audio_update() runs from the PIT tick and calls hda_update(), which
 * reads the stream's position (LPIB) and refills the buffers the
 * hardware has already played with samples pulled from audio_manager.
 *
 * Setup:
 *   1. Reset the controller and find the codecs (STATESTS).
 *   2. Talk to the codecs through the CORB/RIRB rings, or the
 *      "immediate command" registers if the rings do not answer.
 *   3. In every codec's audio function group, walk back from each
 *      analog output pin (speaker, line out, headphone) through
 *      mixers/selectors to a DAC, unmuting and selecting along the way.
 *      Every DAC found gets the same stream tag, so all outputs play.
 *   4. Program the first output stream descriptor with a cyclic buffer
 *      of HDA_BUF_COUNT buffers described by a BDL.
 *
 * HDA only plays a fixed set of rates (multiples of 44.1/48 kHz). When
 * the codec cannot play the file's rate, this driver resamples to a
 * rate it can (linear interpolation) - real codecs often lack 22.05
 * or 32 kHz.
 */

#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include "x86_64/audio_manager.h"
#include "x86_64/hda_driver.h"
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/port.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "string.h"

#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_HDA     0x03

/* ---- Controller registers ---- */
#define HDA_GCAP      0x00
#define HDA_GCTL      0x08
#define HDA_STATESTS  0x0E
#define HDA_INTCTL    0x20
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP    0x48
#define HDA_CORBRP    0x4A
#define HDA_CORBCTL   0x4C
#define HDA_CORBSIZE  0x4E
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP    0x58
#define HDA_RINTCNT   0x5A
#define HDA_RIRBCTL   0x5C
#define HDA_RIRBSTS   0x5D
#define HDA_RIRBSIZE  0x5E
#define HDA_ICOI      0x60
#define HDA_ICII      0x64
#define HDA_ICIS      0x68

#define HDA_GCTL_CRST   (1u << 0)
#define HDA_CORBCTL_RUN (1u << 1)
#define HDA_RIRBCTL_DMA (1u << 1)
#define HDA_RIRBCTL_INT (1u << 0)   /* response "interrupt" status; GIE stays off */
#define HDA_ICIS_BUSY   (1u << 0)
#define HDA_ICIS_VALID  (1u << 1)

/* ---- Stream descriptor registers (offset from the descriptor) ---- */
#define HDA_SD_CTL   0x00
#define HDA_SD_STS   0x03
#define HDA_SD_LPIB  0x04
#define HDA_SD_CBL   0x08
#define HDA_SD_LVI   0x0C
#define HDA_SD_FMT   0x12
#define HDA_SD_BDPL  0x18
#define HDA_SD_BDPU  0x1C

#define HDA_SD_CTL_SRST (1u << 0)
#define HDA_SD_CTL_RUN  (1u << 1)
#define HDA_STREAM_TAG  1u

/* ---- Codec verbs and parameters ---- */
#define VERB_GET_PARAM      0xF00
#define VERB_GET_CONN_LIST  0xF02
#define VERB_SET_CONN_SEL   0x701
#define VERB_SET_POWER      0x705
#define VERB_SET_STREAM     0x706
#define VERB_SET_PIN_CTL    0x707
#define VERB_SET_EAPD       0x70C
#define VERB_GET_CONFIG_DEF 0xF1C
#define VERB4_SET_FORMAT    0x2
#define VERB4_SET_AMP       0x3

#define PARAM_VENDOR_ID    0x00
#define PARAM_NODE_COUNT   0x04
#define PARAM_FG_TYPE      0x05
#define PARAM_WIDGET_CAP   0x09
#define PARAM_PCM          0x0A
#define PARAM_PIN_CAP      0x0C
#define PARAM_IN_AMP_CAP   0x0D
#define PARAM_CONN_LEN     0x0E
#define PARAM_OUT_AMP_CAP  0x12

#define WCAP_IN_AMP       (1u << 1)
#define WCAP_OUT_AMP      (1u << 2)
#define WCAP_AMP_OVERRIDE (1u << 3)
#define WCAP_FMT_OVERRIDE (1u << 4)
#define WCAP_DIGITAL      (1u << 9)
#define WCAP_POWER_CTRL   (1u << 10)
#define WCAP_TYPE(c)      (((c) >> 20) & 0xF)

enum { W_DAC = 0, W_ADC = 1, W_MIXER = 2, W_SELECTOR = 3, W_PIN = 4 };

#define PINCAP_OUT  (1u << 4)
#define PINCAP_EAPD (1u << 16)
#define PIN_CTL_OUT (1u << 6)
#define PIN_CTL_HP  (1u << 7)

/* ---- Buffers ---- */
#define HDA_BUF_COUNT   8
#define HDA_BUF_FRAMES  AUDIO_BUFFER_SIZE
#define HDA_BUF_BYTES   (HDA_BUF_FRAMES * AUDIO_CHANNELS * (uint32_t)sizeof(int16_t))
/* Same fill-ahead depth as AC97 and the player's decode ring. */
#define HDA_TARGET_AHEAD AUDIO_HW_RING_SIZE

#define HDA_MAX_WIDGETS 128
#define HDA_MAX_CONN    16
#define HDA_MAX_DACS    8

typedef struct {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed)) hda_bdl_entry_t;

typedef struct {
    uint8_t  cad;
    uint8_t  nid;
} hda_node_t;

static volatile uint8_t* g_mmio = NULL;
static uint32_t g_sd = 0;                    /* output stream descriptor offset */

static volatile uint32_t* g_corb = NULL;
static volatile uint32_t* g_rirb = NULL;
static uint16_t g_corb_entries = 0;
static uint16_t g_rirb_entries = 0;
static uint16_t g_corb_wp = 0;
static uint16_t g_rirb_rp = 0;
static bool     g_immediate = false;

static hda_bdl_entry_t* g_bdl = NULL;
static int16_t* g_bufs[HDA_BUF_COUNT];

static hda_node_t g_dacs[HDA_MAX_DACS];
static int        g_dac_count = 0;
static uint32_t   g_rate_mask = 0;           /* PARAM_PCM rate bits every DAC supports */

static volatile bool g_ready   = false;
static volatile bool g_running = false;
static uint32_t g_write = 0;                 /* next buffer to fill */

/* Rates: requested by the player (source) and played by the codec. */
static uint32_t g_src_rate = 48000;
static uint32_t g_hw_rate  = 48000;
static uint16_t g_hw_fmt   = 0;

/* Linear resampler state (used when g_src_rate != g_hw_rate). */
static int16_t  g_src_buf[HDA_BUF_FRAMES * AUDIO_CHANNELS];
static uint32_t g_src_idx = HDA_BUF_FRAMES;  /* frames consumed from g_src_buf */
static uint32_t g_step = 0x10000;            /* source frames per output frame, 16.16 */
static uint32_t g_frac = 0;
static int16_t  g_cur[2], g_nxt[2];

/* ============================================================
 * Low-level helpers
 * ============================================================ */

static inline uint8_t  r8(uint32_t o)  { return *(volatile uint8_t*)(g_mmio + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t*)(g_mmio + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t*)(g_mmio + o); }
static inline void w8(uint32_t o, uint8_t v)   { *(volatile uint8_t*)(g_mmio + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(g_mmio + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(g_mmio + o) = v; }

/* About 1 µs per iteration; works before interrupts are enabled. */
static void hda_udelay(uint32_t us) {
    while (us--) port_io_wait();
}

static void pci_cfg_write32(pci_device_t* d, uint8_t off, uint32_t v) {
    uint32_t addr = (1u << 31) | ((uint32_t)d->bus << 16) |
                    ((uint32_t)d->device << 11) | ((uint32_t)d->function << 8) |
                    (off & 0xFC);
    port_outl(0xCF8, addr);
    port_outl(0xCFC, v);
}

static inline uint64_t dma_addr(const void* p) {
    /* Kernel heap is identity mapped (AC97 and AHCI rely on this too). */
    return (uint64_t)(uintptr_t)p;
}

/* ============================================================
 * Codec commands
 * ============================================================ */

static bool hda_cmd_immediate(uint32_t verb, uint32_t* resp) {
    uint32_t t = 10000;
    while ((r16(HDA_ICIS) & HDA_ICIS_BUSY) && t--) hda_udelay(1);
    w16(HDA_ICIS, (uint16_t)(r16(HDA_ICIS) | HDA_ICIS_VALID));   /* clear old */
    w32(HDA_ICOI, verb);
    w16(HDA_ICIS, (uint16_t)(r16(HDA_ICIS) | HDA_ICIS_BUSY));
    for (t = 10000; t; t--) {
        uint16_t s = r16(HDA_ICIS);
        if ((s & (HDA_ICIS_VALID | HDA_ICIS_BUSY)) == HDA_ICIS_VALID) {
            if (resp) *resp = r32(HDA_ICII);
            return true;
        }
        hda_udelay(1);
    }
    return false;
}

static bool hda_cmd_corb(uint32_t verb, uint32_t* resp) {
    uint16_t wp = (uint16_t)((g_corb_wp + 1) % g_corb_entries);
    g_corb[wp] = verb;
    __asm__ volatile("" ::: "memory");
    g_corb_wp = wp;
    w16(HDA_CORBWP, wp);

    for (uint32_t t = 10000; t; t--) {
        uint16_t rwp = (uint16_t)(r16(HDA_RIRBWP) & 0xFF);
        while (rwp != g_rirb_rp) {
            g_rirb_rp = (uint16_t)((g_rirb_rp + 1) % g_rirb_entries);
            uint32_t r  = g_rirb[g_rirb_rp * 2];
            uint32_t ex = g_rirb[g_rirb_rp * 2 + 1];
            /* Clear the response flags; with RINTCNT=1 some controllers
             * (QEMU among them) wait for this before the next command. */
            w8(HDA_RIRBSTS, 0x05);
            if (ex & (1u << 4)) continue;      /* unsolicited response */
            if (resp) *resp = r;
            return true;
        }
        hda_udelay(1);
    }
    return false;
}

static uint32_t hda_cmd(uint8_t cad, uint8_t nid, uint32_t verb, uint32_t payload) {
    uint32_t v = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) |
                 ((verb & 0xFFF) << 8) | (payload & 0xFF);
    uint32_t r = 0;
    bool ok = g_immediate ? hda_cmd_immediate(v, &r) : hda_cmd_corb(v, &r);
    return ok ? r : 0;
}

/* 4-bit verbs carry a 16-bit payload (format, amplifier gain). */
static uint32_t hda_cmd4(uint8_t cad, uint8_t nid, uint32_t verb, uint32_t payload) {
    uint32_t v = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) |
                 ((verb & 0xF) << 16) | (payload & 0xFFFF);
    uint32_t r = 0;
    bool ok = g_immediate ? hda_cmd_immediate(v, &r) : hda_cmd_corb(v, &r);
    return ok ? r : 0;
}

static uint32_t hda_param(uint8_t cad, uint8_t nid, uint8_t param) {
    return hda_cmd(cad, nid, VERB_GET_PARAM, param);
}

/* ============================================================
 * Controller bring-up
 * ============================================================ */

static bool hda_reset_controller(void) {
    w8(HDA_CORBCTL, 0);
    w8(HDA_RIRBCTL, 0);

    w32(HDA_GCTL, r32(HDA_GCTL) & ~HDA_GCTL_CRST);
    uint32_t t = 10000;
    while ((r32(HDA_GCTL) & HDA_GCTL_CRST) && --t) hda_udelay(1);
    hda_udelay(100);

    w32(HDA_GCTL, r32(HDA_GCTL) | HDA_GCTL_CRST);
    t = 10000;
    while (!(r32(HDA_GCTL) & HDA_GCTL_CRST) && --t) hda_udelay(1);
    if (!(r32(HDA_GCTL) & HDA_GCTL_CRST)) {
        serial_write_str("HDA: controller did not leave reset\n");
        return false;
    }

    /* Codecs announce themselves within 521 µs of leaving reset. */
    for (t = 0; t < 20 && r16(HDA_STATESTS) == 0; t++) hda_udelay(1000);
    w32(HDA_INTCTL, 0);   /* polled driver */
    return true;
}

static uint16_t hda_ring_entries(uint8_t size_reg, uint8_t* sel) {
    uint8_t cap = (uint8_t)(size_reg >> 4);
    if (cap & 0x4) { *sel = 2; return 256; }
    if (cap & 0x2) { *sel = 1; return 16; }
    *sel = 0; return 2;
}

static bool hda_setup_rings(void) {
    /* Allocated once; a later controller (after one was given up) reuses
     * them. */
    if (!g_corb) g_corb = (volatile uint32_t*)kmalloc_aligned(1024, 128);
    if (!g_rirb) g_rirb = (volatile uint32_t*)kmalloc_aligned(2048, 128);
    if (!g_corb || !g_rirb) return false;
    memset((void*)g_corb, 0, 1024);
    memset((void*)g_rirb, 0, 2048);

    uint8_t sel;
    uint32_t t;

    /* CORB */
    w8(HDA_CORBCTL, 0);
    for (t = 1000; (r8(HDA_CORBCTL) & HDA_CORBCTL_RUN) && t; t--) hda_udelay(1);
    w32(HDA_CORBLBASE, (uint32_t)dma_addr((const void*)g_corb));
    w32(HDA_CORBUBASE, (uint32_t)(dma_addr((const void*)g_corb) >> 32));
    g_corb_entries = hda_ring_entries(r8(HDA_CORBSIZE), &sel);
    w8(HDA_CORBSIZE, (uint8_t)((r8(HDA_CORBSIZE) & 0xFC) | sel));
    w16(HDA_CORBWP, 0);
    /* Read pointer reset: set bit 15, wait for it, clear it. Some
     * controllers never show the bit, so timeouts are not fatal. */
    w16(HDA_CORBRP, 0x8000);
    for (t = 1000; !(r16(HDA_CORBRP) & 0x8000) && t; t--) hda_udelay(1);
    w16(HDA_CORBRP, 0);
    for (t = 1000; (r16(HDA_CORBRP) & 0x8000) && t; t--) hda_udelay(1);
    g_corb_wp = 0;

    /* RIRB */
    w8(HDA_RIRBCTL, 0);
    for (t = 1000; (r8(HDA_RIRBCTL) & HDA_RIRBCTL_DMA) && t; t--) hda_udelay(1);
    w32(HDA_RIRBLBASE, (uint32_t)dma_addr((const void*)g_rirb));
    w32(HDA_RIRBUBASE, (uint32_t)(dma_addr((const void*)g_rirb) >> 32));
    g_rirb_entries = hda_ring_entries(r8(HDA_RIRBSIZE), &sel);
    w8(HDA_RIRBSIZE, (uint8_t)((r8(HDA_RIRBSIZE) & 0xFC) | sel));
    w16(HDA_RIRBWP, 0x8000);       /* reset write pointer */
    w16(HDA_RINTCNT, 1);
    g_rirb_rp = 0;

    /* The RIRBSTS response bit is only set with RIRBCTL bit 0 on, and
     * controllers such as QEMU's stop after RINTCNT responses until
     * software clears it. No IRQ fires: INTCTL's global enable is off. */
    w8(HDA_RIRBCTL, HDA_RIRBCTL_DMA | HDA_RIRBCTL_INT);
    w8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    hda_udelay(100);
    return true;
}

/* ============================================================
 * Codec path setup
 * ============================================================ */

typedef struct {
    uint8_t  cad;
    uint8_t  afg;
    uint8_t  first;              /* first widget nid */
    uint8_t  count;
    uint32_t wcap[HDA_MAX_WIDGETS];
    uint8_t  conn[HDA_MAX_WIDGETS][HDA_MAX_CONN];
    uint8_t  nconn[HDA_MAX_WIDGETS];
    bool     visited[HDA_MAX_WIDGETS];
} hda_codec_t;

static hda_codec_t* g_codec = NULL;   /* scratch, reused per codec */

static bool hda_widget_index(const hda_codec_t* c, uint8_t nid, int* idx) {
    if (nid < c->first || nid >= c->first + c->count) return false;
    *idx = nid - c->first;
    return true;
}

static int hda_read_connections(hda_codec_t* c, uint8_t nid, uint8_t* out) {
    uint32_t len_param = hda_param(c->cad, nid, PARAM_CONN_LEN);
    bool long_form = (len_param & 0x80) != 0;
    uint32_t len = len_param & 0x7F;
    int n = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < len;) {
        uint32_t resp = hda_cmd(c->cad, nid, VERB_GET_CONN_LIST, i);
        uint32_t per = long_form ? 2 : 4;
        for (uint32_t j = 0; j < per && i < len; j++, i++) {
            uint32_t e = long_form ? (resp >> (16 * j)) & 0xFFFF
                                   : (resp >> (8 * j)) & 0xFF;
            bool range = long_form ? (e & 0x8000) : (e & 0x80);
            uint32_t v = long_form ? (e & 0x7FFF) : (e & 0x7F);
            if (range && prev) {
                for (uint32_t x = prev + 1; x <= v && n < HDA_MAX_CONN; x++)
                    out[n++] = (uint8_t)x;
            } else if (n < HDA_MAX_CONN) {
                out[n++] = (uint8_t)v;
            }
            prev = v;
        }
    }
    return n;
}

static void hda_unmute(hda_codec_t* c, uint8_t nid, bool output, uint8_t index) {
    int idx;
    if (!hda_widget_index(c, nid, &idx)) return;
    uint32_t cap = hda_param(c->cad, nid, output ? PARAM_OUT_AMP_CAP : PARAM_IN_AMP_CAP);
    if (!(c->wcap[idx] & WCAP_AMP_OVERRIDE) || cap == 0)
        cap = hda_param(c->cad, c->afg, output ? PARAM_OUT_AMP_CAP : PARAM_IN_AMP_CAP);
    uint32_t steps  = (cap >> 8) & 0x7F;
    uint32_t offset = cap & 0x7F;           /* the 0 dB step */
    uint32_t gain   = offset > steps ? steps : offset;
    uint32_t payload = (output ? 0x8000u : 0x4000u) | 0x3000u |
                       ((uint32_t)(index & 0xF) << 8) | gain;
    hda_cmd4(c->cad, nid, VERB4_SET_AMP, payload);
}

static void hda_power_up(hda_codec_t* c, uint8_t nid) {
    int idx;
    if (!hda_widget_index(c, nid, &idx)) return;
    if (c->wcap[idx] & WCAP_POWER_CTRL) hda_cmd(c->cad, nid, VERB_SET_POWER, 0);
}

static void hda_add_dac(hda_codec_t* c, uint8_t nid) {
    for (int i = 0; i < g_dac_count; i++)
        if (g_dacs[i].cad == c->cad && g_dacs[i].nid == nid) return;
    if (g_dac_count >= HDA_MAX_DACS) return;

    int idx;
    hda_widget_index(c, nid, &idx);
    uint32_t pcm = 0;
    if (c->wcap[idx] & WCAP_FMT_OVERRIDE) pcm = hda_param(c->cad, nid, PARAM_PCM);
    if (!(pcm & 0xFFF)) pcm = hda_param(c->cad, c->afg, PARAM_PCM);
    uint32_t rates = pcm & 0xFFF;
    if (!rates) rates = (1u << 5) | (1u << 6);       /* assume 44.1k and 48k */
    g_rate_mask = g_dac_count ? (g_rate_mask & rates) : rates;
    if (!g_rate_mask) g_rate_mask = (1u << 6);

    hda_power_up(c, nid);
    hda_unmute(c, nid, true, 0);
    hda_cmd(c->cad, nid, VERB_SET_STREAM, (HDA_STREAM_TAG << 4) | 0);
    g_dacs[g_dac_count].cad = c->cad;
    g_dacs[g_dac_count].nid = nid;
    g_dac_count++;
}

/* Depth-first search from `nid` toward a DAC. On the way back out it
 * selects and unmutes the input that leads to the DAC. */
static uint8_t hda_find_dac(hda_codec_t* c, uint8_t nid, int depth) {
    int idx;
    if (!hda_widget_index(c, nid, &idx)) return 0;
    uint32_t type = WCAP_TYPE(c->wcap[idx]);
    if (type == W_DAC) return (c->wcap[idx] & WCAP_DIGITAL) ? 0 : nid;
    if (depth > 0 && type != W_MIXER && type != W_SELECTOR) return 0;
    if (depth > 6 || c->visited[idx]) return 0;
    c->visited[idx] = true;

    for (int i = 0; i < c->nconn[idx]; i++) {
        uint8_t dac = hda_find_dac(c, c->conn[idx][i], depth + 1);
        if (!dac) continue;
        hda_power_up(c, nid);
        if (type != W_MIXER && c->nconn[idx] > 1)
            hda_cmd(c->cad, nid, VERB_SET_CONN_SEL, (uint32_t)i);
        if (c->wcap[idx] & WCAP_IN_AMP)
            hda_unmute(c, nid, false, type == W_MIXER ? (uint8_t)i : 0);
        if (c->wcap[idx] & WCAP_OUT_AMP)
            hda_unmute(c, nid, true, 0);
        return dac;
    }
    return 0;
}

/* Sets up one output pin. Returns true if it reached a DAC. */
static bool hda_setup_pin(hda_codec_t* c, uint8_t pin) {
    int idx;
    hda_widget_index(c, pin, &idx);
    memset(c->visited, 0, sizeof(c->visited));
    uint8_t dac = hda_find_dac(c, pin, 0);
    if (!dac) return false;

    uint32_t config = hda_cmd(c->cad, pin, VERB_GET_CONFIG_DEF, 0);
    uint32_t device = (config >> 20) & 0xF;
    uint32_t pincap = hda_param(c->cad, pin, PARAM_PIN_CAP);

    hda_cmd(c->cad, pin, VERB_SET_PIN_CTL, PIN_CTL_OUT | (device == 2 ? PIN_CTL_HP : 0));
    if (pincap & PINCAP_EAPD) hda_cmd(c->cad, pin, VERB_SET_EAPD, 0x02);
    hda_add_dac(c, dac);

    serial_write_str("HDA: codec ");
    serial_write_dec(c->cad);
    serial_write_str(" pin ");
    serial_write_dec(pin);
    serial_write_str(" (device ");
    serial_write_dec(device);
    serial_write_str(") -> DAC ");
    serial_write_dec(dac);
    serial_write_str("\n");
    return true;
}

static void hda_setup_codec(uint8_t cad) {
    hda_codec_t* c = g_codec;
    memset(c, 0, sizeof(*c));
    c->cad = cad;

    uint32_t vendor = hda_param(cad, 0, PARAM_VENDOR_ID);
    serial_write_str("HDA: codec ");
    serial_write_dec(cad);
    serial_write_str(" vendor ");
    serial_write_hex(vendor);
    serial_write_str("\n");

    uint32_t nodes = hda_param(cad, 0, PARAM_NODE_COUNT);
    uint8_t fg_first = (uint8_t)((nodes >> 16) & 0xFF);
    uint8_t fg_count = (uint8_t)(nodes & 0xFF);

    for (uint8_t fg = fg_first; fg < fg_first + fg_count; fg++) {
        if ((hda_param(cad, fg, PARAM_FG_TYPE) & 0xFF) != 0x01) continue;  /* audio FG */
        c->afg = fg;
        hda_cmd(cad, fg, VERB_SET_POWER, 0);
        hda_udelay(1000);

        uint32_t w = hda_param(cad, fg, PARAM_NODE_COUNT);
        c->first = (uint8_t)((w >> 16) & 0xFF);
        c->count = (uint8_t)(w & 0xFF);
        if (c->count > HDA_MAX_WIDGETS) c->count = HDA_MAX_WIDGETS;

        for (int i = 0; i < c->count; i++) {
            uint8_t nid = (uint8_t)(c->first + i);
            c->wcap[i] = hda_param(cad, nid, PARAM_WIDGET_CAP);
            if (c->wcap[i] & (1u << 8))   /* has a connection list */
                c->nconn[i] = (uint8_t)hda_read_connections(c, nid, c->conn[i]);
        }

        /* Preferred outputs first: speaker, line out, headphone. Pins
         * marked "no physical connection" are skipped unless nothing
         * else works (QEMU and some laptops leave the config blank). */
        static const uint32_t order[] = { 1, 0, 2 };
        int configured = 0;
        for (int pass = 0; pass < 2 && !configured; pass++) {
            for (int o = 0; o < 3; o++) {
                for (int i = 0; i < c->count; i++) {
                    uint32_t cap = c->wcap[i];
                    if (WCAP_TYPE(cap) != W_PIN || (cap & WCAP_DIGITAL)) continue;
                    uint8_t nid = (uint8_t)(c->first + i);
                    if (!(hda_param(cad, nid, PARAM_PIN_CAP) & PINCAP_OUT)) continue;
                    uint32_t config = hda_cmd(cad, nid, VERB_GET_CONFIG_DEF, 0);
                    uint32_t device = (config >> 20) & 0xF;
                    uint32_t connectivity = (config >> 30) & 0x3;
                    if (pass == 0) {
                        if (device != order[o] || connectivity == 1) continue;
                    } else if (o != 0) {
                        continue;    /* second pass: any output pin, once */
                    }
                    if (hda_setup_pin(c, nid)) configured++;
                }
            }
        }
    }
}

/* ============================================================
 * Stream format and rates
 * ============================================================ */

typedef struct { uint32_t rate; uint16_t fmt; uint8_t cap_bit; } hda_rate_t;

/* fmt bits: 14 = 44.1 kHz base, 13:11 = multiplier-1, 10:8 = divisor-1 */
static const hda_rate_t g_rates[] = {
    {  8000, (0u << 14) | (0u << 11) | (5u << 8), 0 },
    { 11025, (1u << 14) | (0u << 11) | (3u << 8), 1 },
    { 16000, (0u << 14) | (0u << 11) | (2u << 8), 2 },
    { 22050, (1u << 14) | (0u << 11) | (1u << 8), 3 },
    { 32000, (0u << 14) | (1u << 11) | (2u << 8), 4 },
    { 44100, (1u << 14) | (0u << 11) | (0u << 8), 5 },
    { 48000, (0u << 14) | (0u << 11) | (0u << 8), 6 },
    { 88200, (1u << 14) | (1u << 11) | (0u << 8), 7 },
    { 96000, (0u << 14) | (1u << 11) | (0u << 8), 8 },
};
#define HDA_RATE_COUNT (sizeof(g_rates) / sizeof(g_rates[0]))

static void hda_choose_rate(uint32_t want) {
    const hda_rate_t* pick = NULL;
    for (uint32_t i = 0; i < HDA_RATE_COUNT; i++)
        if (g_rates[i].rate == want && (g_rate_mask & (1u << g_rates[i].cap_bit)))
            pick = &g_rates[i];
    if (!pick) {
        /* Resample to 48 kHz, else 44.1 kHz, else the highest rate offered. */
        static const uint8_t fallback[] = { 6, 5, 8, 7, 4, 3, 2, 1, 0 };
        for (uint32_t i = 0; i < sizeof(fallback) && !pick; i++)
            if (g_rate_mask & (1u << g_rates[fallback[i]].cap_bit))
                pick = &g_rates[fallback[i]];
        if (!pick) pick = &g_rates[6];
    }
    g_hw_rate = pick->rate;
    /* 16-bit samples (bits 6:4 = 1), stereo (channels-1 = 1) */
    g_hw_fmt = (uint16_t)(pick->fmt | (1u << 4) | (AUDIO_CHANNELS - 1));
    g_step = (uint32_t)(((uint64_t)g_src_rate << 16) / g_hw_rate);
}

/* ============================================================
 * Sample rendering (with optional resampling)
 * ============================================================ */

static void hda_resampler_reset(void) {
    g_src_idx = HDA_BUF_FRAMES;
    g_frac = 0;
    g_cur[0] = g_cur[1] = 0;
    g_nxt[0] = g_nxt[1] = 0;
}

static void hda_next_src_frame(void) {
    if (g_src_idx >= HDA_BUF_FRAMES) {
        audio_manager_pull_samples(g_src_buf, HDA_BUF_FRAMES);
        g_src_idx = 0;
    }
    g_cur[0] = g_nxt[0];
    g_cur[1] = g_nxt[1];
    g_nxt[0] = g_src_buf[g_src_idx * 2];
    g_nxt[1] = g_src_buf[g_src_idx * 2 + 1];
    g_src_idx++;
}

static void hda_render(int16_t* out, uint32_t frames) {
    if (g_step == 0x10000) {
        audio_manager_pull_samples(out, frames);
        return;
    }
    for (uint32_t i = 0; i < frames; i++) {
        int64_t f = (int64_t)g_frac;
        out[i * 2]     = (int16_t)(g_cur[0] + (((int64_t)(g_nxt[0] - g_cur[0]) * f) >> 16));
        out[i * 2 + 1] = (int16_t)(g_cur[1] + (((int64_t)(g_nxt[1] - g_cur[1]) * f) >> 16));
        g_frac += g_step;
        while (g_frac >= 0x10000) {
            g_frac -= 0x10000;
            hda_next_src_frame();
        }
    }
}

/* ============================================================
 * Stream control
 * ============================================================ */

static void hda_stream_stop(void) {
    uint32_t ctl = r32(g_sd + HDA_SD_CTL) & 0x00FFFFFF;
    w32(g_sd + HDA_SD_CTL, ctl & ~HDA_SD_CTL_RUN);
    for (uint32_t t = 1000; (r32(g_sd + HDA_SD_CTL) & HDA_SD_CTL_RUN) && t; t--)
        hda_udelay(1);
}

static void hda_stream_program(void) {
    hda_stream_stop();

    uint32_t ctl = r32(g_sd + HDA_SD_CTL) & 0x00FFFFFF;
    w32(g_sd + HDA_SD_CTL, ctl | HDA_SD_CTL_SRST);
    for (uint32_t t = 1000; !(r32(g_sd + HDA_SD_CTL) & HDA_SD_CTL_SRST) && t; t--)
        hda_udelay(1);
    ctl = r32(g_sd + HDA_SD_CTL) & 0x00FFFFFF;
    w32(g_sd + HDA_SD_CTL, ctl & ~HDA_SD_CTL_SRST);
    for (uint32_t t = 1000; (r32(g_sd + HDA_SD_CTL) & HDA_SD_CTL_SRST) && t; t--)
        hda_udelay(1);

    w8(g_sd + HDA_SD_STS, 0x1C);
    w32(g_sd + HDA_SD_BDPL, (uint32_t)dma_addr(g_bdl));
    w32(g_sd + HDA_SD_BDPU, (uint32_t)(dma_addr(g_bdl) >> 32));
    w32(g_sd + HDA_SD_CBL, HDA_BUF_COUNT * HDA_BUF_BYTES);
    w16(g_sd + HDA_SD_LVI, HDA_BUF_COUNT - 1);
    w16(g_sd + HDA_SD_FMT, g_hw_fmt);

    ctl = r32(g_sd + HDA_SD_CTL) & 0x00FFFFFF;
    ctl = (ctl & ~0x00F00000u) | (HDA_STREAM_TAG << 20);
    w32(g_sd + HDA_SD_CTL, ctl);

    for (int i = 0; i < g_dac_count; i++)
        hda_cmd4(g_dacs[i].cad, g_dacs[i].nid, VERB4_SET_FORMAT, g_hw_fmt);
}

static void hda_start(void) {
    if (!g_ready) return;
    g_running = false;

    hda_choose_rate(g_src_rate);
    hda_stream_program();
    hda_resampler_reset();

    for (int i = 0; i < HDA_BUF_COUNT; i++)
        memset(g_bufs[i], 0, HDA_BUF_BYTES);
    for (int i = 0; i < HDA_TARGET_AHEAD; i++)
        hda_render(g_bufs[i], HDA_BUF_FRAMES);
    g_write = HDA_TARGET_AHEAD % HDA_BUF_COUNT;

    uint32_t ctl = r32(g_sd + HDA_SD_CTL) & 0x00FFFFFF;
    w32(g_sd + HDA_SD_CTL, ctl | HDA_SD_CTL_RUN);
    g_running = true;

    serial_write_str("HDA: playing at ");
    serial_write_dec(g_hw_rate);
    serial_write_str(" Hz");
    if (g_hw_rate != g_src_rate) {
        serial_write_str(" (resampled from ");
        serial_write_dec(g_src_rate);
        serial_write_str(" Hz)");
    }
    serial_write_str("\n");
}

static void hda_update(void) {
    if (!g_running) return;

    uint32_t lpib = r32(g_sd + HDA_SD_LPIB) % (HDA_BUF_COUNT * HDA_BUF_BYTES);
    uint32_t hw = lpib / HDA_BUF_BYTES;

    /* The stream loops over the ring forever, so a buffer that is not
     * refilled plays again. When the player has nothing, keep a couple
     * of buffers of silence ahead instead of letting old audio repeat. */
    for (;;) {
        uint32_t lead = (g_write + HDA_BUF_COUNT - hw) % HDA_BUF_COUNT;
        if (lead >= HDA_TARGET_AHEAD) break;
        if (lead >= 2 && !audio_manager_has_data()) break;
        hda_render(g_bufs[g_write], HDA_BUF_FRAMES);
        g_write = (g_write + 1) % HDA_BUF_COUNT;
    }
}

static void hda_set_sample_rate(uint32_t rate_hz) {
    if (rate_hz < 8000)  rate_hz = 8000;
    if (rate_hz > 96000) rate_hz = 96000;
    g_src_rate = rate_hz;
    /* Applied by the next hda_start(); audio_player_play() always
     * calls start right after this. Restart now if already playing. */
    if (g_running) hda_start();
}

static void hda_shutdown(void) {
    if (!g_ready) return;
    g_running = false;
    hda_stream_stop();
}

/* ============================================================
 * Init
 * ============================================================ */

static volatile uint8_t* hda_map_bar0(pci_device_t* dev) {
    uint64_t phys = dev->bar_address[0] ? dev->bar_address[0]
                                        : (uint64_t)(dev->bar[0] & ~0xFu);
    uint64_t size = dev->bar_length[0] ? dev->bar_length[0] : 0x4000;
    if (!phys) return NULL;

    uint64_t base = phys & ~0xFFFull;
    uint64_t total = ((phys - base) + size + 0xFFF) & ~0xFFFull;
    uintptr_t va = (uintptr_t)mmio_alloc_va(total);
    if (!va) return NULL;
    for (uint64_t off = 0; off < total; off += 0x1000)
        if (!map_mmio_page(va + off, (uintptr_t)(base + off), 0x1000)) return NULL;
    return (volatile uint8_t*)(va + (phys - base));
}

/* Stops a controller that was given up, so it no longer reads or writes
 * the command rings the next one will use. */
static void hda_abandon(void) {
    if (!g_mmio) return;
    w8(HDA_CORBCTL, 0);
    w8(HDA_RIRBCTL, 0);
    w32(HDA_GCTL, r32(HDA_GCTL) & ~HDA_GCTL_CRST);
    g_mmio = NULL;
}

/* HD Audio controllers have a memory BAR0. Intel ones with the audio DSP
 * enabled report the "audio device" subclass (0x01) instead of HDA (0x03);
 * AC97 controllers share that subclass but have I/O BARs. */
static bool hda_candidate(const pci_device_t* d) {
    return d->class_code == PCI_CLASS_MULTIMEDIA &&
           (d->subclass == PCI_SUBCLASS_HDA || d->subclass == 0x01) &&
           d->bar_type[0] == PCI_BAR_MEM;
}

/* Graphics cards' HDMI/DisplayPort audio: tried last, as it has no
 * speaker or headphone jacks. */
static bool hda_is_gpu_audio(const pci_device_t* d) {
    return d->vendor_id == 0x10DE || (d->vendor_id == 0x1002 &&
           d->device_id != 0x437B && d->device_id != 0x4383);  /* ATI SB450/SB600+ */
}

/* Lets the controller see what the CPU wrote to its buffers (cache snoop),
 * as Linux does: PCIe "no snoop" off, and AMD's own snoop enable. */
static void hda_enable_snoop(pci_device_t* dev) {
    uint8_t pcie = pci_find_capability(dev, 0x10);
    if (pcie) {
        uint32_t v = pci_config_read(dev->bus, dev->device, dev->function, (uint8_t)(pcie + 8));
        if (v & (1u << 11)) pci_cfg_write32(dev, (uint8_t)(pcie + 8), v & ~(1u << 11));
    }
    if (dev->vendor_id == 0x1022 || dev->vendor_id == 0x1002) {
        /* ATI_SB450_HDAUDIO_MISC_CNTR2 (0x42): bits 2:0 = 2, snoop on. */
        uint32_t v = pci_config_read(dev->bus, dev->device, dev->function, 0x40);
        v = (v & ~(0x07u << 16)) | (0x02u << 16);
        pci_cfg_write32(dev, 0x40, v);
    }
}

static bool hda_try_controller(pci_device_t* dev) {
    g_immediate = false;
    g_dac_count = 0;
    g_mmio = NULL;
    pci_enable_io_busmaster(dev);
    hda_enable_snoop(dev);
    if (dev->vendor_id == 0x8086) {
        /* Intel: TCSEL (config 0x44 bits 2:0) to TC0, as Linux does,
         * or some chipsets play crackly or stall. */
        uint32_t v = pci_config_read(dev->bus, dev->device, dev->function, 0x44);
        pci_cfg_write32(dev, 0x44, v & ~0x7u);
    }

    g_mmio = hda_map_bar0(dev);
    if (!g_mmio) {
        serial_write_str("HDA: Failed to map BAR0\n");
        return false;
    }

    serial_write_str("HDA: controller ");
    serial_write_hex(dev->vendor_id);
    serial_write_str(":");
    serial_write_hex(dev->device_id);
    serial_write_str("\n");

    if (!hda_reset_controller()) return false;

    uint16_t gcap = r16(HDA_GCAP);
    uint32_t iss = (gcap >> 8) & 0xF;
    uint32_t oss = (gcap >> 12) & 0xF;
    if (oss == 0) {
        serial_write_str("HDA: controller has no output streams\n");
        return false;
    }
    g_sd = 0x80 + iss * 0x20;

    uint16_t codecs = r16(HDA_STATESTS);
    if (!codecs) {
        serial_write_str("HDA: no codecs found\n");
        return false;
    }

    if (!hda_setup_rings()) {
        serial_write_str("HDA: ring alloc failed\n");
        return false;
    }

    /* Check the CORB/RIRB answer; otherwise use immediate commands. */
    uint8_t first_cad = 0;
    while (first_cad < 15 && !(codecs & (1u << first_cad))) first_cad++;
    uint32_t probe = 0;
    uint32_t v = ((uint32_t)first_cad << 28) | (VERB_GET_PARAM << 8) | PARAM_VENDOR_ID;
    if (!hda_cmd_corb(v, &probe)) {
        serial_write_str("HDA: CORB/RIRB not answering, using immediate commands\n");
        w8(HDA_CORBCTL, 0);
        w8(HDA_RIRBCTL, 0);
        g_immediate = true;
        if (!hda_cmd_immediate(v, &probe)) {
            serial_write_str("HDA: codec does not answer\n");
            return false;
        }
    }

    g_codec = (hda_codec_t*)alloc(sizeof(hda_codec_t));
    if (!g_codec) return false;
    g_dac_count = 0;
    for (uint8_t cad = 0; cad < 15; cad++)
        if (codecs & (1u << cad)) hda_setup_codec(cad);
    free_mem(g_codec);
    g_codec = NULL;

    if (g_dac_count == 0) {
        serial_write_str("HDA: no usable analog output found\n");
        return false;
    }

    g_bdl = (hda_bdl_entry_t*)kmalloc_aligned(HDA_BUF_COUNT * sizeof(hda_bdl_entry_t), 128);
    uint8_t* pcm = (uint8_t*)kmalloc_aligned(HDA_BUF_COUNT * HDA_BUF_BYTES, 4096);
    if (!g_bdl || !pcm) {
        serial_write_str("HDA: buffer alloc failed\n");
        return false;
    }
    memset(pcm, 0, HDA_BUF_COUNT * HDA_BUF_BYTES);
    for (int i = 0; i < HDA_BUF_COUNT; i++) {
        g_bufs[i] = (int16_t*)(pcm + i * HDA_BUF_BYTES);
        g_bdl[i].addr_lo = (uint32_t)dma_addr(g_bufs[i]);
        g_bdl[i].addr_hi = (uint32_t)(dma_addr(g_bufs[i]) >> 32);
        g_bdl[i].length  = HDA_BUF_BYTES;
        g_bdl[i].flags   = 0;      /* polled: no interrupt on completion */
    }

    g_ready = true;
    hda_choose_rate(g_src_rate);
    hda_stream_program();

    serial_write_str("HDA: Initialized, ");
    serial_write_dec((uint64_t)g_dac_count);
    serial_write_str(" DAC(s)\n");
    return true;
}


static bool hda_init(void) {
    serial_write_str("HDA: Searching for HD Audio controller...\n");
    if (pci_get_device_count() == 0) pci_enumerate_all();

    /* Every controller is tried until one has a usable analog output:
     * the first one found is often a graphics card's HDMI audio. */
    bool any = false;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < pci_get_device_count(); i++) {
            pci_device_t* dev = &pci_devices[i];
            if (!hda_candidate(dev) || hda_is_gpu_audio(dev) != (pass == 1)) continue;
            any = true;
            if (hda_try_controller(dev)) return true;
            hda_abandon();
        }
    }
    serial_write_str(any ? "HDA: no controller with a usable output\n"
                         : "HDA: No controller found\n");
    return false;
}

static const audio_hw_driver_t hda_driver = {
    .name            = "Intel HD Audio",
    .init            = hda_init,
    .start           = hda_start,
    .update          = hda_update,
    .set_sample_rate = hda_set_sample_rate,
    .shutdown        = hda_shutdown,
};

const audio_hw_driver_t* hda_get_driver(void) {
    return &hda_driver;
}
