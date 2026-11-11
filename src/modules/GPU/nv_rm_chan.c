#include <stdarg.h>
#include "x86_64/nv_gpu.h"
#include "x86_64/nv_rm.h"
#include "x86_64/nv_nvdec.h"
#include "x86_64/gpucore.h"
#include "x86_64/pmm.h"
#include "x86_64/scheduler.h"
#include "string.h"
#include "time.h"
#include "serial.h"

/*
 * Step 3b: channels through GSP-RM. Step 3c: the copy engine fills
 * rectangles in VRAM (the first pixels on the screen, FGPU clears).
 * Step 4a: the 3D engine (TURING_A) on its own channel.
 *
 * Order, as nouveau's r535 code does it:
 *   1. RM objects: client (NV01_ROOT), device (NV01_DEVICE_0), subdevice.
 *   2. FIFO device info table: which copy engines exist, their runlists,
 *      and the graphics runlist.
 *   3. VA space (FERMI_VASPACE_A). We build the GPU page tables in VRAM
 *      ourselves and tell GSP-RM about the top three levels
 *      (COPY_SERVER_RESERVED_PDES), leaving it one 512 MiB range of its own.
 *   4. Channel (TURING_CHANNEL_GPFIFO_A) on a copy engine: instance block
 *      and USERD in VRAM, GPFIFO ring and pushbuffer in sysmem. BIND to
 *      the engine, SCHEDULE. Then the copy engine object (TURING_DMA_COPY_A).
 *   5. Graphics (gr/r535.c): RM's internal client tells the sizes of the
 *      graphics context buffers. A throwaway "golden" channel gets all of
 *      them promoted and a 3D object, which makes RM record the golden
 *      context; the global buffers stay. The 3D channel then gets its own
 *      main and patch buffers plus the global ones, and the 3D object.
 *   6. Submit: GPFIFO entry, GP_PUT in USERD, doorbell. A semaphore in
 *      sysmem tells when the work is done.
 *
 * GPU virtual addresses (one VA space for all channels):
 *   0x20000000  512 MiB managed by GSP-RM (its page tables below our PD1)
 *   0x40000000  sysmem, 64 KiB per channel: GPFIFO ring, pushbuffer,
 *               semaphore (4 KiB pages)
 *   0x80000000  VRAM offset 0 onwards, as much as the BAR1 window shows
 *               (2 MiB pages): FGPU's VRAM offsets and the graphics
 *               context buffers map 1:1 onto it.
 *
 * VRAM is written through the BAR1 window, which maps VRAM 1:1 while BAR1
 * is in physical mode (nv_bar1_keep_physical keeps it there).
 */

#define H_CLIENT   0xC1D0A001u
#define H_DEVICE   0xDE1D0001u
#define H_SUBDEV   0x5D1D0001u
#define H_VASPACE  0x90F10001u
#define H_CHANNEL  0xF1F00000u      // | channel id
#define H_CE       0xCEB50001u
#define H_3D       0x3D970001u
#define H_3D_GOLDEN 0x3D970002u
#define H_NVDEC     0xDEC00001u

#define VA_RM_RSVD 0x20000000ull
#define VA_RM_SIZE 0x20000000ull
#define VA_SYS     0x40000000ull
#define VA_VRAM    0x80000000ull
#define VRAM_MAP_MAX (2ull << 30)   // 4 PD0 tables of 2 MiB pages
#define PAGE_2M    (2ull << 20)

// Per channel, behind its sysmem VA (4 KiB pages).
#define SYS_GPFIFO   0u             // 1 page
#define SYS_PUSH     1u             // 4 pages
#define SYS_SEM      5u             // 1 page
#define SYS_PAGES    6u
#define SYS_STRIDE   16u            // pages of VA per channel
#define GPFIFO_ENTRIES 128u
#define PUSH_WORDS   (4u * 4096u / 4u)

// Staging: system memory the copy engine copies VRAM into, so the CPU
// never reads VRAM through BAR1 (uncached, very slow). Mapped once, at
// VA_STAGE, when the page tables are built.
#define VA_STAGE     (VA_SYS + (16ull << 20))
#define STAGE_MAX_PAGES 4096u       // 16 MiB: a 1920x1080 frame fits
#define STAGE_MIN_PAGES 256u

#define RPC_TIMEOUT_MS 5000u
#define MAX_CTXBUF 10

typedef struct {
    const char* name;
    uint32_t slot;                      // sysmem VA slot
    uint32_t handle;                    // RM handle, 0 = none
    uint32_t engine, runlist, chid, token;
    uint64_t inst, userd;               // VRAM
    uint8_t* sys;                       // SYS_PAGES pages, identity-mapped
    uint8_t* mthdbuf;
    uint32_t gp_put, payload;
} chan_t;

typedef struct {
    uint16_t id;                        // NV_PROMOTE_*
    uint32_t size;
    uint8_t align;                      // log2
    bool global, init;
    uint64_t vram;                      // the global copy (from the golden promote)
} ctxbuf_t;

static chan_t g_ce = { .name = "copy engine", .slot = 0 }, g_gr = { .name = "3D", .slot = 1 },
              g_golden = { .name = "golden context", .slot = 2 },
              g_dec = { .name = "video decoder", .slot = 3 };

static struct {
    gpu_adapter_t* a;
    bool ready;                         // copy engine channel works
    bool gr_ready;                      // 3D channel works
    bool gr_tried;
    bool dec_ready, dec_tried;          // NVDEC channel works
    uint32_t dec_runlist;
    uint32_t jobs_dec;
    bool client, device, subdevice, vaspace, pdes;

    void (*print)(const char* text);

    uint64_t pd3, pd2, pd1, pd0_sys, pt_sys, pd0_vram[4];
    uint64_t pt_stage[STAGE_MAX_PAGES / 512u];
    uint8_t* stage;                     // staging memory, NULL = none
    uint32_t stage_pages;
    uint32_t reads;
    uint64_t vram_mapped;               // bytes of VRAM mapped at VA_VRAM
    uint32_t mthdbuf_size;
    uint32_t gr_runlist;
    uint32_t chids_used;                // bit per entry of k_chids

    ctxbuf_t ctx[MAX_CTXBUF];
    uint32_t ctx_count;

    uint32_t fills, clears3d, jobs3d;
} c;

static const uint32_t k_chids[] = { 0, 8, 16, 24, 32, 64 };

static uint8_t g_buf[4096];             // RPC payload assembly

/* ---------------------------------------------------------- utilities */

static void say(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (c.print) c.print(line);
    serial_write_str(line);
}

static void wr(uint32_t reg, uint32_t v) { nv_wr32(c.a, reg, v); }
static uint64_t pa(const void* p) { return (uint64_t)(uintptr_t)p; }

// Write-combined VRAM writes reach the card in order before anything we
// write to the registers afterwards.
static void wc_flush(void) { __asm__ volatile("sfence" ::: "memory"); }

static void vram_wr32(uint64_t off, uint32_t v) { *(volatile uint32_t*)(c.a->aperture + off) = v; }
static uint32_t vram_rd32(uint64_t off) { return *(volatile uint32_t*)(c.a->aperture + off); }
static void vram_wr64(uint64_t off, uint64_t v) {
    vram_wr32(off, (uint32_t)v);
    vram_wr32(off + 4, (uint32_t)(v >> 32));
}

static bool vram_alloc(uint64_t size, uint64_t align, uint64_t* out, const char* what) {
    uint64_t off = gpu_vram_alloc(c.a, size, align, 0);
    if (off == GPU_VRAM_INVALID) {
        say("  out of VRAM for the %s (%llu KiB)\n", what, (unsigned long long)(size >> 10));
        return false;
    }
    *out = off;
    return true;
}

static bool vram_page(uint64_t* out, const char* what) {
    if (!vram_alloc(4096, 4096, out, what)) return false;
    for (uint32_t i = 0; i < 4096; i += 4) vram_wr32(*out + i, 0);
    return true;
}

static uint64_t chan_va_sys(const chan_t* ch) { return VA_SYS + (uint64_t)ch->slot * SYS_STRIDE * 4096u; }

/* -------------------------------------------------------------- RM calls */

static uint32_t rm_alloc(uint32_t parent, uint32_t handle, uint32_t cls, const void* params, uint32_t size,
                         const char* what) {
    nv_rm_alloc_rpc_t* h = (nv_rm_alloc_rpc_t*)g_buf;
    memset(g_buf, 0, sizeof(*h) + size);
    h->hClient = cls == NV_CLASS_ROOT ? handle : H_CLIENT;
    h->hParent = parent;
    h->hObject = handle;
    h->hClass = cls;
    h->paramsSize = size;
    if (size) memcpy(h + 1, params, size);
    nv_rpc_hdr_t* r = (nv_rpc_hdr_t*)nv_gsp_rpc(c.a, NV_RPC_GSP_RM_ALLOC, g_buf, (uint32_t)sizeof(*h) + size,
                                                RPC_TIMEOUT_MS);
    if (!r) { say("  %s: no answer from GSP-RM\n", what); return 0xFFFFFFFFu; }
    const nv_rm_alloc_rpc_t* rep = (const nv_rm_alloc_rpc_t*)(r + 1);
    uint32_t status = r->rpc_result ? r->rpc_result : rep->status;
    if (status) say("  %s (class 0x%x): RM error 0x%x\n", what, cls, status);
    return status;
}

// Returns the reply's parameters, NULL on error.
static void* rm_control_as(uint32_t client, uint32_t object, uint32_t cmd, const void* params, uint32_t size,
                           const char* what) {
    nv_rm_control_rpc_t* h = (nv_rm_control_rpc_t*)g_buf;
    if (sizeof(*h) + size > sizeof(g_buf)) return NULL;
    memset(g_buf, 0, sizeof(*h) + size);
    h->hClient = client;
    h->hObject = object;
    h->cmd = cmd;
    h->paramsSize = size;
    if (size) memcpy(h + 1, params, size);
    nv_rpc_hdr_t* r = (nv_rpc_hdr_t*)nv_gsp_rpc(c.a, NV_RPC_GSP_RM_CONTROL, g_buf, (uint32_t)sizeof(*h) + size,
                                                RPC_TIMEOUT_MS);
    if (!r) { say("  %s: no answer from GSP-RM\n", what); return NULL; }
    nv_rm_control_rpc_t* rep = (nv_rm_control_rpc_t*)(r + 1);
    uint32_t status = r->rpc_result ? r->rpc_result : rep->status;
    if (status) {
        say("  %s (control 0x%x): RM error 0x%x\n", what, cmd, status);
        return NULL;
    }
    return rep + 1;
}

static void* rm_control(uint32_t object, uint32_t cmd, const void* params, uint32_t size, const char* what) {
    return rm_control_as(H_CLIENT, object, cmd, params, size, what);
}

static void rm_free(uint32_t parent, uint32_t handle) {
    nv_rm_free_rpc_t f = { H_CLIENT, parent, handle, 0 };
    nv_gsp_rpc(c.a, NV_RPC_FREE, &f, sizeof(f), RPC_TIMEOUT_MS);
}

/* ------------------------------------------------------------ objects */

static bool make_objects(void) {
    if (!c.client) {
        nv_root_alloc_t p;
        memset(&p, 0, sizeof(p));
        p.hClient = H_CLIENT;
        p.processID = 0xFFFFFFFFu;
        strcpy(p.processName, "Minimal-OS");
        if (rm_alloc(H_CLIENT, H_CLIENT, NV_CLASS_ROOT, &p, sizeof(p), "client")) return false;
        c.client = true;
    }
    if (!c.device) {
        nv_device_alloc_t p;
        memset(&p, 0, sizeof(p));
        p.hClientShare = H_CLIENT;
        if (rm_alloc(H_CLIENT, H_DEVICE, NV_CLASS_DEVICE, &p, sizeof(p), "device")) return false;
        c.device = true;
    }
    if (!c.subdevice) {
        nv_subdevice_alloc_t p = { 0 };
        if (rm_alloc(H_DEVICE, H_SUBDEV, NV_CLASS_SUBDEVICE, &p, sizeof(p), "subdevice")) return false;
        c.subdevice = true;
    }
    say("  RM objects: client 0x%x, device, subdevice\n", H_CLIENT);
    return true;
}

typedef struct {
    uint32_t engine, runlist;
} ce_t;

// Copy engines from the FIFO device info table, those that do not share
// the graphics runlist first (a GRCE would need the graphics context).
// Also notes the graphics runlist.
static uint32_t find_copy_engines(ce_t* out, uint32_t cap) {
    static nv_fifo_device_info_t info;
    ce_t all[32];
    c.dec_runlist = 0xFFFFFFFFu;
    uint32_t n = 0, gr_runlist = 0xFFFFFFFFu;
    for (uint32_t base = 0; base < 256;) {
        memset(&info, 0, sizeof(info));
        info.baseIndex = base;
        nv_fifo_device_info_t* r = (nv_fifo_device_info_t*)rm_control(
            H_SUBDEV, NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE, &info, sizeof(info), "FIFO device info");
        if (!r) return 0;
        uint32_t entries = r->numEntries < 32 ? r->numEntries : 32;
        for (uint32_t i = 0; i < entries; i++) {
            const uint32_t* d = r->entries[i].engineData;
            uint32_t type = d[NV_ENGINE_INFO_RM_ENGINE_TYPE];
            if (type == NV_ENGINE_GR0) gr_runlist = d[NV_ENGINE_INFO_RUNLIST];
            if (type == NV_ENGINE_NVDEC0) c.dec_runlist = d[NV_ENGINE_INFO_RUNLIST];
            if (type >= NV_ENGINE_COPY0 && type <= NV_ENGINE_COPY9 && n < 32)
                all[n++] = (ce_t){ type, d[NV_ENGINE_INFO_RUNLIST] };
        }
        if (!r->bMore || !entries) break;
        base += entries;
    }
    c.gr_runlist = gr_runlist;
    uint32_t k = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n && k < cap; i++)
            if ((all[i].runlist == gr_runlist) == (pass == 1)) out[k++] = all[i];
    char line[160];
    int used = snprintf(line, sizeof(line), "  copy engines:");
    for (uint32_t i = 0; i < k && used < (int)sizeof(line) - 24; i++)
        used += snprintf(line + used, sizeof(line) - (size_t)used, " CE%u (runlist %u%s)",
                         out[i].engine - NV_ENGINE_COPY0, out[i].runlist,
                         out[i].runlist == gr_runlist ? ", graphics" : "");
    say("%s\n", line);
    return k;
}

/* --------------------------------------------------- GPU page tables */

static bool build_page_tables(void) {
    if (!vram_page(&c.pd3, "page directory") || !vram_page(&c.pd2, "page directory") ||
        !vram_page(&c.pd1, "page directory") || !vram_page(&c.pd0_sys, "page directory") ||
        !vram_page(&c.pt_sys, "page table"))
        return false;

    vram_wr64(c.pd3 + 8u * nv_gmmu_index(VA_SYS, 4), nv_gmmu_pde(c.pd2, NV_GMMU_PDE_VRAM));
    vram_wr64(c.pd2 + 8u * nv_gmmu_index(VA_SYS, 3), nv_gmmu_pde(c.pd1, NV_GMMU_PDE_VRAM));
    // PD1 entry for VA_RM_RSVD stays empty: GSP-RM fills it.
    vram_wr64(c.pd1 + 8u * nv_gmmu_index(VA_SYS, 2), nv_gmmu_pde(c.pd0_sys, NV_GMMU_PDE_VRAM));
    // PD0 entries are 16 bytes: big-page table, then small-page table.
    vram_wr64(c.pd0_sys + 16u * nv_gmmu_index(VA_SYS, 1) + 8, nv_gmmu_pde(c.pt_sys, NV_GMMU_PDE_VRAM));

    // Sysmem for every channel, mapped now so no PTE changes later.
    chan_t* chans[4] = { &g_ce, &g_gr, &g_golden, &g_dec };
    for (int k = 0; k < 4; k++) {
        chan_t* ch = chans[k];
        ch->sys = (uint8_t*)pmm_try_alloc_pages_zeroed(SYS_PAGES);
        if (!ch->sys) { say("  out of memory for the %s channel's GPFIFO and pushbuffer\n", ch->name); return false; }
        for (uint32_t i = 0; i < SYS_PAGES; i++)
            vram_wr64(c.pt_sys + 8u * nv_gmmu_index(chan_va_sys(ch) + i * 4096ull, 0),
                      nv_gmmu_pte(pa(ch->sys + i * 4096u), NV_GMMU_APER_SYS_COH));
    }

    // Staging memory: one page table per 2 MiB.
    for (uint32_t pages = STAGE_MAX_PAGES; pages >= STAGE_MIN_PAGES && !c.stage; pages /= 2u) {
        c.stage = (uint8_t*)pmm_try_alloc_pages_zeroed(pages);
        if (c.stage) c.stage_pages = pages;
    }
    for (uint32_t i = 0; i < c.stage_pages; i++) {
        uint64_t va = VA_STAGE + (uint64_t)i * 4096u;
        if ((i % 512u) == 0) {
            uint64_t pt;
            if (!vram_page(&pt, "page table")) { c.stage_pages = i; break; }
            vram_wr64(c.pd0_sys + 16u * nv_gmmu_index(va, 1) + 8, nv_gmmu_pde(pt, NV_GMMU_PDE_VRAM));
            c.pt_stage[i / 512u] = pt;
        }
        vram_wr64(c.pt_stage[i / 512u] + 8u * nv_gmmu_index(va, 0),
                  nv_gmmu_pte(pa(c.stage + (uint64_t)i * 4096u), NV_GMMU_APER_SYS_COH));
    }
    if (!c.stage_pages) c.stage = NULL;

    // VRAM 1:1 at VA_VRAM in 2 MiB pages, PTEs straight in PD0.
    uint64_t size = c.a->aperture_size;
    if (size > VRAM_MAP_MAX) size = VRAM_MAP_MAX;
    size &= ~(PAGE_2M - 1);
    uint32_t tables = (uint32_t)((size + (512ull << 20) - 1) >> 29);
    for (uint32_t t = 0; t < tables; t++) {
        if (!vram_page(&c.pd0_vram[t], "page directory")) return false;
        uint64_t va = VA_VRAM + ((uint64_t)t << 29);
        vram_wr64(c.pd1 + 8u * nv_gmmu_index(va, 2), nv_gmmu_pde(c.pd0_vram[t], NV_GMMU_PDE_VRAM));
    }
    for (uint64_t off = 0; off < size; off += PAGE_2M) {
        uint64_t va = VA_VRAM + off;
        uint64_t pd0 = c.pd0_vram[(va - VA_VRAM) >> 29];
        vram_wr64(pd0 + 16u * nv_gmmu_index(va, 1), nv_gmmu_pte(off, NV_GMMU_APER_VRAM));
    }
    c.vram_mapped = size;
    wc_flush();
    (void)vram_rd32(c.pd3);
    say("  page tables: PD3 at VRAM 0x%llx; sysmem at VA 0x%llx, VRAM 0..%llu MiB at VA 0x%llx\n",
        (unsigned long long)c.pd3, (unsigned long long)VA_SYS, (unsigned long long)(size >> 20),
        (unsigned long long)VA_VRAM);
    return true;
}

static bool make_vaspace(void) {
    if (!c.vaspace) {
        nv_vaspace_alloc_t p;
        memset(&p, 0, sizeof(p));
        if (rm_alloc(H_DEVICE, H_VASPACE, NV_CLASS_VASPACE, &p, sizeof(p), "VA space")) return false;
        c.vaspace = true;
    }
    if (c.pdes) return true;
    nv_copy_pdes_t p;
    memset(&p, 0, sizeof(p));
    p.pageSize = VA_RM_SIZE;
    p.virtAddrLo = VA_RM_RSVD;
    p.virtAddrHi = VA_RM_RSVD + VA_RM_SIZE - 1;
    p.numLevelsToCopy = 3;
    p.levels[0].physAddress = c.pd3; p.levels[0].size = 0x20;   p.levels[0].aperture = 1; p.levels[0].pageShift = 47;
    p.levels[1].physAddress = c.pd2; p.levels[1].size = 0x1000; p.levels[1].aperture = 1; p.levels[1].pageShift = 38;
    p.levels[2].physAddress = c.pd1; p.levels[2].size = 0x1000; p.levels[2].aperture = 1; p.levels[2].pageShift = 29;
    if (!rm_control(H_VASPACE, NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES, &p, sizeof(p),
                    "VA space page directories"))
        return false;
    c.pdes = true;
    say("  VA space: GSP-RM manages VA 0x%llx-0x%llx\n", (unsigned long long)VA_RM_RSVD,
        (unsigned long long)(VA_RM_RSVD + VA_RM_SIZE - 1));
    return true;
}

/* ------------------------------------------------------------- channels */

static bool chan_memory(chan_t* ch) {
    if (!ch->inst && !vram_page(&ch->inst, "channel instance block")) return false;
    if (!ch->userd && !vram_page(&ch->userd, "channel USERD")) return false;
    if (!c.mthdbuf_size) {
        uint32_t* size = (uint32_t*)rm_control(H_SUBDEV, NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE,
                                               &(uint32_t){ 0 }, 4, "method buffer size");
        c.mthdbuf_size = size && *size ? *size : 0x10000u;
        c.mthdbuf_size = (c.mthdbuf_size + 4095u) & ~4095u;
    }
    if (!ch->mthdbuf) {
        ch->mthdbuf = (uint8_t*)pmm_try_alloc_pages_zeroed(c.mthdbuf_size / 4096u);
        if (!ch->mthdbuf) { say("  out of memory for the method buffer\n"); return false; }
    }
    return true;
}

// Allocates the channel on `engine` with channel id `chid`; with
// `schedule`, binds it to the engine and lets it run.
static bool chan_alloc(chan_t* ch, uint32_t engine, uint32_t runlist, uint32_t chid, bool schedule) {
    // USERD and the instance block start zeroed for every attempt.
    for (uint32_t i = 0; i < 4096; i += 4) { vram_wr32(ch->inst + i, 0); vram_wr32(ch->userd + i, 0); }
    wc_flush();

    nv_channel_alloc_t p;
    memset(&p, 0, sizeof(p));
    p.gpFifoOffset = chan_va_sys(ch) + SYS_GPFIFO * 4096ull;
    p.gpFifoEntries = GPFIFO_ENTRIES;
    p.flags = NVOS04_FLAGS_PRIVILEGED_CHANNEL | NVOS04_FLAGS_USERD_INDEX(chid % 8u) |
              NVOS04_FLAGS_USERD_INDEX_PAGE(chid / 8u) | NVOS04_FLAGS_USERD_INDEX_PAGE_FIXED;
    p.hVASpace = H_VASPACE;
    p.engineType = engine;
    p.instanceMem = (nv_mem_desc_t){ ch->inst, NV_CHAN_INST_SIZE, NV_ADDR_FBMEM, 1 };
    p.userdMem = (nv_mem_desc_t){ ch->userd, NV_CHAN_USERD_SIZE, NV_ADDR_FBMEM, 1 };
    p.ramfcMem = (nv_mem_desc_t){ ch->inst, NV_CHAN_RAMFC_SIZE, NV_ADDR_FBMEM, 1 };
    p.mthdbufMem = (nv_mem_desc_t){ pa(ch->mthdbuf), c.mthdbuf_size, NV_ADDR_SYSMEM, 0 };
    p.internalFlags = NV_CHANNEL_INTERNAL_ADMIN_NO_NOTIFIERS;

    uint32_t handle = H_CHANNEL | chid;
    if (rm_alloc(H_DEVICE, handle, NV_CLASS_TURING_CHANNEL_GPFIFO_A, &p, sizeof(p), "channel")) return false;
    if (schedule) {
        nv_gpfifo_schedule_t sched = { 1, 0 };
        if (!rm_control(handle, NVA06F_CTRL_CMD_BIND, &engine, sizeof(engine), "channel bind") ||
            !rm_control(handle, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sched, sizeof(sched), "channel schedule")) {
            rm_free(H_DEVICE, handle);
            return false;
        }
    }
    ch->handle = handle;
    ch->engine = engine;
    ch->runlist = runlist;
    ch->chid = chid;
    ch->token = (runlist << 16) | chid;
    ch->gp_put = 0;
    return true;
}

// Tries the free channel ids in turn.
static bool chan_open(chan_t* ch, uint32_t engine, uint32_t runlist, bool schedule) {
    if (!chan_memory(ch)) return false;
    for (uint32_t i = 0; i < sizeof(k_chids) / sizeof(k_chids[0]); i++) {
        if (c.chids_used & (1u << i)) continue;
        if (chan_alloc(ch, engine, runlist, k_chids[i], schedule)) {
            c.chids_used |= 1u << i;
            return true;
        }
    }
    return false;
}

static void chan_close(chan_t* ch) {
    if (!ch->handle) return;
    rm_free(H_DEVICE, ch->handle);
    for (uint32_t i = 0; i < sizeof(k_chids) / sizeof(k_chids[0]); i++)
        if (k_chids[i] == ch->chid) c.chids_used &= ~(1u << i);
    ch->handle = 0;
}

static bool make_ce_channel(const ce_t* ces, uint32_t count) {
    for (uint32_t e = 0; e < count; e++) {
        if (!chan_open(&g_ce, ces[e].engine, ces[e].runlist, true)) continue;
        nv_ce_alloc_t cep = { 1, ces[e].engine };
        if (rm_alloc(g_ce.handle, H_CE, NV_CLASS_TURING_DMA_COPY_A, &cep, sizeof(cep), "copy engine object")) {
            chan_close(&g_ce);
            continue;
        }
        say("  channel %u on CE%u (runlist %u), doorbell token 0x%x\n", g_ce.chid,
            g_ce.engine - NV_ENGINE_COPY0, g_ce.runlist, g_ce.token);
        return true;
    }
    say("  no copy engine accepted a channel\n");
    return false;
}

/* -------------------------------------------------------------- submit */

static volatile uint32_t* semaphore(chan_t* ch) { return (volatile uint32_t*)(ch->sys + SYS_SEM * 4096u); }
static uint32_t* push_words(chan_t* ch) { return (uint32_t*)(ch->sys + SYS_PUSH * 4096u); }
static uint64_t sem_va(chan_t* ch) { return chan_va_sys(ch) + SYS_SEM * 4096ull; }

static bool submit_and_wait(chan_t* ch, uint32_t words, uint32_t payload, uint32_t timeout_ms) {
    uint32_t* ring = (uint32_t*)(ch->sys + SYS_GPFIFO * 4096u);
    nv_gpfifo_entry(chan_va_sys(ch) + SYS_PUSH * 4096ull, words, &ring[ch->gp_put * 2u]);
    ch->gp_put = (ch->gp_put + 1u) % GPFIFO_ENTRIES;
    __asm__ volatile("mfence" ::: "memory");
    vram_wr32(ch->userd + NV_USERD_GP_PUT, ch->gp_put);
    wc_flush();
    (void)vram_rd32(ch->userd + NV_USERD_GP_PUT);
    wr(NV_USERMODE_BASE + NV_USERMODE_NOTIFY_CHANNEL_PENDING, ch->token);

    uint64_t end = time_get_uptime_ms() + timeout_ms;
    uint64_t next_poll = 0;
    while (*semaphore(ch) != payload) {
        uint64_t now = time_get_uptime_ms();
        if (now >= end) {
            say("  %s: no semaphore after %u ms (got 0x%x, want 0x%x); GP_GET %u, GP_PUT %u\n", ch->name,
                timeout_ms, *semaphore(ch), payload, vram_rd32(ch->userd + NV_USERD_GP_GET), ch->gp_put);
            nv_gsp_rm_poll(c.a);
            return false;
        }
        if (now >= next_poll) {     // GSP-RM reports channel errors as events
            nv_gsp_rm_poll(c.a);
            next_poll = now + 20;
        }
        __asm__ volatile("pause");
    }
    return true;
}

static bool fill_locked(uint64_t vram_offset, uint32_t width, uint32_t height, uint32_t pitch_bytes,
                        uint32_t value) {
    nv_push_t p;
    nv_push_init(&p, push_words(&g_ce), PUSH_WORDS);
    uint32_t payload = ++g_ce.payload;
    nv_push_ce_fill(&p, VA_VRAM + vram_offset, width, height, pitch_bytes, value, sem_va(&g_ce), payload);
    if (p.overflow) return false;
    wc_flush();                     // CPU writes to the surface land first
    if (!submit_and_wait(&g_ce, p.count, payload, 2000)) {
        // The channel is most likely stuck: stop offering work to it, so
        // FGPU goes back to the CPU instead of waiting on every clear.
        c.ready = false;
        say("  copy engine channel disabled after a failed job (%u done before)\n", c.fills);
        return false;
    }
    c.fills++;
    return true;
}

static bool clear3d_locked(uint64_t vram_offset, uint32_t width, uint32_t height, uint32_t pitch_bytes,
                           uint32_t xrgb) {
    nv_push_t p;
    nv_push_init(&p, push_words(&g_gr), PUSH_WORDS);
    uint32_t payload = ++g_gr.payload;
    nv_push_clear_xrgb(&p, VA_VRAM + vram_offset, width, height, pitch_bytes, xrgb);
    nv_push_host_semaphore(&p, sem_va(&g_gr), payload);
    if (p.overflow) return false;
    wc_flush();
    if (!submit_and_wait(&g_gr, p.count, payload, 2000)) {
        c.gr_ready = false;
        say("  3D channel disabled after a failed job (%u done before)\n", c.clears3d);
        return false;
    }
    c.clears3d++;
    return true;
}

static volatile int g_busy;      // one job at a time; others fall back to the CPU

static bool lock(void) { return !__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE); }
static void unlock(void) { __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE); }

static bool rect_ok(uint64_t vram_offset, uint32_t width, uint32_t height, uint32_t pitch_bytes) {
    if (!width || !height || pitch_bytes < width * 4u) return false;
    uint64_t end = vram_offset + (uint64_t)(height - 1) * pitch_bytes + (uint64_t)width * 4u;
    return end <= c.vram_mapped;
}

bool nv_rm_ce_fill(gpu_adapter_t* a, uint64_t vram_offset, uint32_t width, uint32_t height,
                   uint32_t pitch_bytes, uint32_t value) {
    if (!c.ready || c.a != a || !rect_ok(vram_offset, width, height, pitch_bytes)) return false;
    if (!lock()) return false;
    nv_bar1_keep_physical(a);       // in case GSP-RM switched it again
    bool ok = fill_locked(vram_offset, width, height, pitch_bytes, value);
    unlock();
    return ok;
}

bool nv_rm_3d_clear(gpu_adapter_t* a, uint64_t vram_offset, uint32_t width, uint32_t height,
                    uint32_t pitch_bytes, uint32_t xrgb) {
    // Pitch-linear render targets: address and pitch 128-byte aligned.
    if (!c.gr_ready || c.a != a || !rect_ok(vram_offset, width, height, pitch_bytes) ||
        (vram_offset & 127u) || (pitch_bytes & 127u) || width > 16384 || height > 16384)
        return false;
    if (!lock()) return false;
    nv_bar1_keep_physical(a);
    bool ok = clear3d_locked(vram_offset, width, height, pitch_bytes, xrgb);
    unlock();
    return ok;
}

/* ------------------------------------------------- 3D jobs (nv_3d.c) */

static nv_push_t g_job;

// Takes the GPU and returns the 3D channel's empty pushbuffer, or NULL
// when the 3D engine is not up or the GPU is busy.
nv_push_t* nv_rm_3d_begin(gpu_adapter_t* a) {
    if (!c.gr_ready || c.a != a || !lock()) return NULL;
    nv_bar1_keep_physical(a);
    nv_push_init(&g_job, push_words(&g_gr), PUSH_WORDS - 8u);    // room for the semaphore
    return &g_job;
}

void nv_rm_3d_cancel(void) { unlock(); }

// Submits what nv_rm_3d_begin's pushbuffer holds and waits for the 3D
// engine to finish it. A job that does not finish switches the 3D path
// off (the copy engine stays).
bool nv_rm_3d_submit(void) {
    uint32_t payload = ++g_gr.payload;
    g_job.capacity = PUSH_WORDS;
    nv_push_host_semaphore(&g_job, sem_va(&g_gr), payload);
    bool ok = !g_job.overflow;
    if (ok) {
        wc_flush();
        ok = submit_and_wait(&g_gr, g_job.count, payload, 2000);
        if (!ok) {
            c.gr_ready = false;
            say("  3D channel disabled after a failed job (%u done before)\n", c.jobs3d);
        } else {
            c.jobs3d++;
        }
    }
    unlock();
    return ok;
}

uint64_t nv_rm_vram_va(uint64_t vram_offset) { return VA_VRAM + vram_offset; }
uint64_t nv_rm_vram_mapped(gpu_adapter_t* a) { return c.a == a ? c.vram_mapped : 0; }
void nv_rm_3d_disable(void) { c.gr_ready = false; }
void nv_rm_say(const char* text) { say("%s", text); }

// Copies `rows` rows of `row_bytes` bytes from VRAM into system memory:
// the copy engine copies to the staging memory, the CPU copies from
// there. False when it does not fit or the GPU did not finish (the
// caller then reads through BAR1).
bool nv_rm_ce_read(gpu_adapter_t* a, uint64_t vram_offset, uint32_t row_bytes, uint32_t rows,
                   uint32_t src_pitch, void* dst, uint32_t dst_pitch) {
    if (!c.ready || c.a != a || !c.stage || !row_bytes || !rows || src_pitch < row_bytes) return false;
    uint32_t stage_pitch = (row_bytes + 63u) & ~63u;
    if ((uint64_t)stage_pitch * rows > (uint64_t)c.stage_pages * 4096u) return false;
    if (vram_offset + (uint64_t)(rows - 1u) * src_pitch + row_bytes > c.vram_mapped) return false;
    if (!lock()) return false;
    nv_bar1_keep_physical(a);
    nv_push_t p;
    nv_push_init(&p, push_words(&g_ce), PUSH_WORDS);
    uint32_t payload = ++g_ce.payload;
    nv_push_ce_copy(&p, VA_VRAM + vram_offset, src_pitch, VA_STAGE, stage_pitch, row_bytes, rows,
                    sem_va(&g_ce), payload);
    wc_flush();
    bool ok = !p.overflow && submit_and_wait(&g_ce, p.count, payload, 2000);
    if (ok) {
        for (uint32_t y = 0; y < rows; y++)
            memcpy((uint8_t*)dst + (size_t)y * dst_pitch, c.stage + (size_t)y * stage_pitch, row_bytes);
        c.reads++;
    } else if (!p.overflow) {
        c.ready = false;
        say("  copy engine channel disabled after a failed read\n");
    }
    unlock();
    return ok;
}

void nv_rm_chan_quiet(void) {
    c.print = NULL;
    nv_gsp_rm_set_print(NULL);
}

/* ------------------------------------------------- graphics context */

static uint8_t log2_ceil(uint64_t v) {
    uint8_t s = 0;
    while ((1ull << s) < v) s++;
    return s;
}

// The buffers RM wants, as nouveau's r535_gr_oneinit lists them.
static bool get_ctxbuf_sizes(void) {
    uint32_t iclient, isub;
    if (!nv_gsp_rm_internal_handles(c.a, &iclient, &isub)) {
        say("  RM's internal handles are unknown (static info)\n");
        return false;
    }
    static nv_ctxbuf_infos_t req;
    memset(&req, 0, sizeof(req));
    nv_ctxbuf_infos_t* info = (nv_ctxbuf_infos_t*)rm_control_as(
        iclient, isub, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO, &req, sizeof(req),
        "graphics context buffer sizes");
    if (!info) return false;

    static const struct { uint8_t engine_id; uint8_t promote_id; bool global, init; } map[] = {
        { NV_CTX_ENGINE_GRAPHICS,        NV_PROMOTE_MAIN,            false, true  },
        { NV_CTX_ENGINE_PATCH,           NV_PROMOTE_PATCH,           false, true  },
        { NV_CTX_ENGINE_BUNDLE_CB,       NV_PROMOTE_BUNDLE_CB,       true,  false },
        { NV_CTX_ENGINE_PAGEPOOL_GLOBAL, NV_PROMOTE_PAGEPOOL,        true,  false },
        { NV_CTX_ENGINE_ATTRIBUTE_CB,    NV_PROMOTE_ATTRIBUTE_CB,    true,  false },
        { NV_CTX_ENGINE_RTV_CB_GLOBAL,   NV_PROMOTE_RTV_CB_GLOBAL,   true,  false },
        { NV_CTX_ENGINE_FECS_EVENT,      NV_PROMOTE_FECS_EVENT,      true,  true  },
        { NV_CTX_ENGINE_PRIV_ACCESS_MAP, NV_PROMOTE_PRIV_ACCESS_MAP, true,  true  },
    };
    c.ctx_count = 0;
    uint64_t total = 0;
    // In the engine id order, as nouveau walks them.
    for (uint32_t id = 0; id < 25; id++) {
        for (uint32_t m = 0; m < sizeof(map) / sizeof(map[0]); m++) {
            if (map[m].engine_id != id) continue;
            uint32_t size = info->engine[0][id].size;
            if (map[m].promote_id == NV_PROMOTE_MAIN) size = ((size + 4095u) & ~4095u) + 64u * 4096u;
            uint8_t page = size >= (1u << 21) ? 21 : size >= (1u << 16) ? 16 : 12;
            uint8_t align = map[m].promote_id == NV_PROMOTE_ATTRIBUTE_CB ? log2_ceil(size) : page;
            if (align < 12) align = 12;
            c.ctx[c.ctx_count++] = (ctxbuf_t){ map[m].promote_id, size, align, map[m].global, map[m].init, 0 };
            total += size;
            if (map[m].promote_id == NV_PROMOTE_PRIV_ACCESS_MAP) {
                ctxbuf_t u = c.ctx[c.ctx_count - 1];
                u.id = NV_PROMOTE_UNRESTRICTED_PRIV_ACCESS_MAP;
                c.ctx[c.ctx_count++] = u;
                total += size;
            }
        }
    }
    say("  graphics context: %u buffers, %llu KiB (main %u KiB)\n", c.ctx_count,
        (unsigned long long)(total >> 10), c.ctx[0].size >> 10);
    return c.ctx_count > 0;
}

// VRAM for a context buffer; zeroed (by the copy engine) when RM wants
// it initialized.
static bool ctxbuf_alloc(const ctxbuf_t* b, uint64_t* out) {
    uint64_t size = ((uint64_t)b->size + 4095u) & ~4095ull;
    uint64_t align = 1ull << b->align;
    if (!vram_alloc(size, align, out, "graphics context")) return false;
    if (*out + size > c.vram_mapped) { say("  graphics context buffer outside the mapped VRAM\n"); return false; }
    if (!b->init) return true;
    while (!lock()) __asm__ volatile("pause");    // FGPU may be using the copy engine
    bool ok = fill_locked(*out, 1024, (uint32_t)(size / 4096u), 4096, 0);
    unlock();
    return ok;
}

// nouveau's r535_gr_promote_ctx: `golden` allocates every buffer (and
// keeps them as the global copies); otherwise only the per-channel ones
// are new and the global ones are shared.
static bool promote(chan_t* ch, bool golden) {
    static nv_promote_ctx_t p;
    memset(&p, 0, sizeof(p));
    p.engineType = NV_ENGINE_GR0;
    p.hChanClient = H_CLIENT;
    p.hObject = ch->handle;
    for (uint32_t i = 0; i < c.ctx_count; i++) {
        ctxbuf_t* b = &c.ctx[i];
        nv_promote_entry_t* e = &p.promoteEntry[p.entryCount];
        bool alloc = golden || !b->global;
        e->bufferId = b->id;
        e->bInitialize = b->init && alloc;
        uint64_t mem;
        if (alloc) {
            if (!ctxbuf_alloc(b, &mem)) return false;
            if (golden) b->vram = mem;
            if (b->id == NV_PROMOTE_PRIV_ACCESS_MAP) e->bNonmapped = 1;
        } else {
            if (b->id == NV_PROMOTE_UNRESTRICTED_PRIV_ACCESS_MAP) continue;
            mem = b->vram;
        }
        if (!e->bNonmapped) e->gpuVirtAddr = VA_VRAM + mem;
        if (e->bInitialize) {
            e->gpuPhysAddr = mem;
            e->size = b->size;
            e->physAttr = 4;
        }
        p.entryCount++;
    }
    return rm_control(H_SUBDEV, NV2080_CTRL_CMD_GPU_PROMOTE_CTX, &p, sizeof(p),
                      golden ? "promote golden context" : "promote 3D context") != NULL;
}

static bool make_3d(void) {
    say("\nStep 4a: 3D engine (TURING_A)\n");
    if (c.gr_runlist == 0xFFFFFFFFu) { say("  no graphics engine in the device info table\n"); return false; }
    if (!get_ctxbuf_sizes()) return false;

    // Golden context, on a throwaway channel.
    if (!chan_open(&g_golden, NV_ENGINE_GR0, c.gr_runlist, false)) {
        say("  no channel for the golden context\n");
        return false;
    }
    bool ok = promote(&g_golden, true) &&
              !rm_alloc(g_golden.handle, H_3D_GOLDEN, NV_CLASS_TURING_A, NULL, 0, "3D object (golden context)");
    if (ok) rm_free(g_golden.handle, H_3D_GOLDEN);
    chan_close(&g_golden);
    if (!ok) return false;
    say("  golden context recorded\n");

    // The 3D channel.
    if (!chan_open(&g_gr, NV_ENGINE_GR0, c.gr_runlist, true)) { say("  no 3D channel\n"); return false; }
    if (!promote(&g_gr, false) || rm_alloc(g_gr.handle, H_3D, NV_CLASS_TURING_A, NULL, 0, "3D object")) {
        chan_close(&g_gr);
        return false;
    }
    say("  channel %u on graphics (runlist %u), doorbell token 0x%x\n", g_gr.chid, g_gr.runlist, g_gr.token);

    // First job: clear a 64x64 surface of our own and read it back.
    uint64_t surf;
    if (!vram_alloc(64 * 256, 4096, &surf, "3D test surface")) return false;
    c.gr_ready = true;
    if (!nv_rm_3d_clear(c.a, surf, 64, 64, 256, 0x00FF8000u)) {
        c.gr_ready = false;
        say("  the first 3D job did not finish\n");
        return false;
    }
    uint32_t got = vram_rd32(surf + 32 * 256 + 32 * 4);
    gpu_vram_free(c.a, surf);
    say("  first 3D clear: reads back 0x%08x (%s)\n", got, got == 0xFFFF8000u ? "ok" : "WRONG");
    if (got != 0xFFFF8000u) { c.gr_ready = false; return false; }
    say("  ok: the 3D engine runs work we submit.\n");
    return true;
}

/* ------------------------------------------------------- video decoder */

// Step 5a: a channel on NVDEC with the decoder class. GSP-RM runs the
// engine's firmware itself; like a copy engine it needs no context from
// us (nouveau: nvkm/engine/nvdec/r535.c).
static bool make_nvdec(void) {
    say("\nStep 5a: video decoder (NVDEC)\n");
    if (c.dec_runlist == 0xFFFFFFFFu) { say("  no video decoder in the device info table\n"); return false; }
    if (!chan_open(&g_dec, NV_ENGINE_NVDEC0, c.dec_runlist, true)) { say("  no channel for the video decoder\n"); return false; }
    // NV_BSP_ALLOCATION_PARAMETERS: size, prohibitMultipleInstances, engineInstance.
    uint32_t params[3] = { sizeof(params), 0, 0 };
    if (rm_alloc(g_dec.handle, H_NVDEC, NV_CLASS_NVDEC_TURING, params, sizeof(params), "video decoder object")) {
        chan_close(&g_dec);
        return false;
    }
    say("  channel %u on NVDEC (runlist %u), doorbell token 0x%x\n", g_dec.chid, g_dec.runlist, g_dec.token);
    // First job: nothing but the semaphore, to see the channel run.
    nv_push_t p;
    nv_push_init(&p, push_words(&g_dec), PUSH_WORDS);
    uint32_t payload = ++g_dec.payload;
    nv_push_1(&p, NV_NVDEC_SUBCHANNEL, 0x0000, NV_CLASS_NVDEC_TURING);
    nv_push_host_semaphore(&p, sem_va(&g_dec), payload);
    while (!lock()) __asm__ volatile("pause");
    bool ok = submit_and_wait(&g_dec, p.count, payload, 2000);
    unlock();
    if (!ok) { say("  the first video decoder job did not finish\n"); chan_close(&g_dec); return false; }
    say("  ok: the video decoder's channel runs.\n");
    return true;
}

static nv_push_t g_dec_job;

bool nv_rm_dec_ready(gpu_adapter_t* a) { return c.dec_ready && c.a == a; }

// Like nv_rm_3d_begin / nv_rm_3d_submit, for the video decoder's channel.
nv_push_t* nv_rm_dec_begin(gpu_adapter_t* a) {
    if (!c.dec_ready || c.a != a || !lock()) return NULL;
    nv_bar1_keep_physical(a);
    nv_push_init(&g_dec_job, push_words(&g_dec), PUSH_WORDS - 8u);
    return &g_dec_job;
}

bool nv_rm_dec_submit(void) {
    uint32_t payload = ++g_dec.payload;
    g_dec_job.capacity = PUSH_WORDS;
    nv_push_host_semaphore(&g_dec_job, sem_va(&g_dec), payload);
    bool ok = !g_dec_job.overflow;
    if (ok) {
        wc_flush();
        ok = submit_and_wait(&g_dec, g_dec_job.count, payload, 2000);
        if (!ok) {
            c.dec_ready = false;
            say("  video decoder channel disabled after a failed job (%u done before)\n", c.jobs_dec);
        } else {
            c.jobs_dec++;
        }
    }
    unlock();
    return ok;
}

void nv_rm_dec_disable(void) { c.dec_ready = false; }

/* ----------------------------------------------------------------- init */

bool nv_rm_chan_ready(gpu_adapter_t* a) { return c.ready && c.a == a; }
bool nv_rm_3d_ready(gpu_adapter_t* a) { return c.gr_ready && c.a == a; }

bool nv_rm_chan_init(gpu_adapter_t* a, void (*print)(const char* text)) {
    if (c.a && c.a != a) { say("  another adapter already has the channels\n"); return false; }
    c.a = a;
    c.print = print;
    nv_gsp_rm_set_print(print);
    if (!c.ready) {
        say("\nStep 3b: RM objects, GPU page tables, copy engine channel\n");
        if (!nv_gsp_rm_running(a)) { say("  GSP-RM is not running\n"); return false; }
        if (!a->aperture || !(a->caps & GPU_CAP_VRAM_HEAP)) {
            say("  no VRAM access through BAR1 (BAR1 in virtual mode?); cannot place GPU structures\n");
            return false;
        }
        nv_bar1_keep_physical(a);
        if (c.pd3 == 0 && !build_page_tables()) return false;
        if (!make_objects()) return false;
        ce_t ces[10];
        uint32_t n = find_copy_engines(ces, 10);
        if (!n) { say("  no copy engines found\n"); return false; }
        if (!make_vaspace()) return false;
        if (!g_ce.handle && !make_ce_channel(ces, n)) return false;
        nv_bar1_keep_physical(a);

        // First job: fill 16 words of our own VRAM page (after USERD's
        // 512 bytes) and read them back.
        c.ready = true;
        if (!nv_rm_ce_fill(a, g_ce.userd + 0x800, 16, 1, 64, 0x12345678u)) {
            c.ready = false;
            say("  the first copy-engine job did not finish\n");
            return false;
        }
        uint32_t got = vram_rd32(g_ce.userd + 0x800);
        say("  first job done: VRAM reads back 0x%08x (%s)\n", got, got == 0x12345678u ? "ok" : "WRONG");
        if (got != 0x12345678u) { c.ready = false; return false; }
        say("  ok: the copy engine runs work we submit.\n");
    }
    // 3D needs the copy engine (it zeroes the context buffers). One try
    // per boot: a half-made graphics context is not retried.
    if (!c.gr_ready && !c.gr_tried) {
        c.gr_tried = true;
        if (!make_3d()) say("  3D engine not available; clears stay on the copy engine\n");
        nv_bar1_keep_physical(a);
    }
    if (!c.dec_ready && !c.dec_tried) {
        c.dec_tried = true;
        c.dec_ready = make_nvdec();
        if (!c.dec_ready) say("  video decoder not available; H.264 is decoded on the CPU\n");
        nv_bar1_keep_physical(a);
    }
    return true;
}

/* ------------------------------------------- step 3c / 4a: the screen */

bool nv_rm_screen_test(gpu_adapter_t* a, void (*print)(const char* text)) {
    c.print = print;
    say("\nStep 3c: GPU writes to the screen\n");
    if (!nv_rm_chan_ready(a)) { say("  the channel is not up\n"); return false; }
    if (!a->scanout_in_vram || !a->scanout_pitch) { say("  the screen is not in VRAM\n"); return false; }
    uint32_t pitch = a->scanout_pitch;
    uint32_t width = pitch / 4u;
    uint32_t height = (uint32_t)(a->scanout_size / pitch);
    if (width < 512 || height < 256) { say("  screen too small\n"); return false; }

    // Four squares in the top right corner: red, green, blue, white
    // (copy engine), and an orange one left of them (3D engine).
    uint32_t side = height / 8u;
    if (side > 160) side = 160;
    uint32_t x0 = width - 2u * side - 16u, y0 = 16u;
    static const uint32_t colors[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    uint64_t t0 = time_get_uptime_ms();
    bool ok = true;
    for (uint32_t i = 0; i < 4 && ok; i++) {
        uint32_t x = x0 + (i % 2u) * side, y = y0 + (i / 2u) * side;
        uint64_t off = a->scanout_offset + (uint64_t)y * pitch + (uint64_t)x * 4u;
        ok = nv_rm_ce_fill(a, off, side, side, pitch, colors[i]);
    }
    uint64_t ms = time_get_uptime_ms() - t0;
    if (!ok) { say("  a fill did not finish\n"); return false; }

    uint32_t good = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t x = x0 + (i % 2u) * side + side / 2u, y = y0 + (i / 2u) * side + side / 2u;
        if (vram_rd32(a->scanout_offset + (uint64_t)y * pitch + (uint64_t)x * 4u) == colors[i]) good++;
    }
    say("  4 squares of %ux%u at (%u,%u), %llu ms; %u of 4 read back right\n", side, side, x0, y0,
        (unsigned long long)ms, good);
    say(good == 4 ? "  ok: red/green/blue/white squares in the top right corner come from the copy engine.\n"
                  : "  the GPU wrote somewhere else (VRAM vs BAR1 offsets?)\n");

    if (nv_rm_3d_ready(a)) {
        // 3D render targets start on a 128-byte boundary: x on 32 pixels.
        uint32_t x = (x0 - side - 16u) & ~31u;
        uint64_t off = a->scanout_offset + (uint64_t)y0 * pitch + (uint64_t)x * 4u;
        bool ok3d = (off & 127u) == 0 && nv_rm_3d_clear(a, off, side, 2u * side, pitch, 0x00FF8000u);
        uint32_t got = vram_rd32(off + (uint64_t)side * pitch + (side / 2u) * 4u);
        say("  3D engine: orange %ux%u at (%u,%u): %s\n", side, 2u * side, x, y0,
            !ok3d ? "did not run" : got == 0xFFFF8000u ? "ok" : "wrong color read back");
    }
    return good == 4;
}
