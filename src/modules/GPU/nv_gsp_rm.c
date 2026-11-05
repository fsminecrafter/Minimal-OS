#include <stdarg.h>
#include "x86_64/nv_gpu.h"
#include "x86_64/nv_rm.h"
#include "x86_64/pmm.h"
#include "x86_64/pci.h"
#include "x86_64/minimafs.h"
#include "x86_64/scheduler.h"
#include "string.h"
#include "time.h"
#include "serial.h"

/*
 * Step 2b, second part: booter_load on SEC2 starts GSP-RM on the GSP
 * RISC-V core. Step 3a: the first RPC round trip with GSP-RM.
 * ('gpu nvidia boot', after FWSEC-FRTS.) Formats and the boot order are
 * described in nv_rm.h; the sequence follows nouveau's r535 GSP code.
 *
 * Every line printed also goes to 0:/gsp_boot.log, saved after each
 * phase: if the screen goes dark during the boot, the log survives a
 * reboot.
 *
 * Sysmem buffers come from the page allocator, whose pages are
 * identity-mapped: the pointer is the physical address the GPU uses.
 * They are allocated once and kept (GSP-RM keeps using them).
 */

static const nv_falcon_t k_gsp = { "GSP", NV_FALCON_GSP };
static const nv_falcon_t k_sec2 = { "SEC2", NV_FALCON_SEC2 };

#define RM_MAX_SLOTS   16u                      // largest message we handle, in 4 KiB slots
#define RM_LOG_PATH    "0:/gsp_boot.log"
#define RM_LOG_SIZE    16384u

typedef struct {
    uint8_t* va;
    uint32_t pages;
} rm_mem_t;

static struct {
    gpu_adapter_t* a;
    bool running;                       // GSP_INIT_DONE received
    bool rpc_ok;                        // step 3a answered
    char gpu_name[64];                  // from GET_GSP_STATIC_INFO, if found

    nv_gsp_layout_t layout;
    nv_rm_bootloader_t bl;
    nv_booter_t booter;
    rm_mem_t booter_img;                // patched booter_load ucode
    rm_mem_t meta, libos, log[3], rmargs, shm, boot, sig, rx3[3];
    uint32_t sig_size;
    uint8_t** elf_pages;
    uint32_t elf_page_count;

    volatile nv_msgq_hdr_t* cmdq;       // our queue to GSP-RM
    volatile nv_msgq_hdr_t* msgq;       // GSP-RM's queue to us
    uint32_t slots;                     // entries per queue
    uint32_t seq;
    rm_mem_t tx, rx;                    // message assembly buffers

    uint32_t events, seq_runs, seq_errors;

    void (*print)(const char* text);
    char log_text[RM_LOG_SIZE];
    uint32_t log_len;
} g;

/* ---------------------------------------------------------- utilities */

static void say(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g.print) g.print(line);
    for (const char* p = line; *p && g.log_len + 1 < RM_LOG_SIZE; p++) g.log_text[g.log_len++] = *p;
}

static void save_log(void) {
    minimafs_delete_file(RM_LOG_PATH);
    if (!minimafs_create_file(RM_LOG_PATH, "text", "txt")) return;
    minimafs_file_handle_t* h = minimafs_open(RM_LOG_PATH, false);
    if (!h) return;
    minimafs_write(h, g.log_text, g.log_len);
    minimafs_close(h);
}

static uint32_t rd(uint32_t reg) { return nv_rd32(g.a, reg); }
static void wr(uint32_t reg, uint32_t v) { nv_wr32(g.a, reg, v); }
static void mask(uint32_t reg, uint32_t m, uint32_t v) { wr(reg, (rd(reg) & ~m) | v); }
static uint32_t frd(const nv_falcon_t* f, uint32_t reg) { return rd(f->base + reg); }
static void fwr(const nv_falcon_t* f, uint32_t reg, uint32_t v) { wr(f->base + reg, v); }

static uint64_t pa(const void* p) { return (uint64_t)(uintptr_t)p; }

// PTIMER counts nanoseconds. Guarded by the uptime clock in case it
// does not tick.
static uint64_t ptimer_ns(void) {
    uint32_t hi, lo;
    do {
        hi = rd(NV_PTIMER_TIME_1);
        lo = rd(NV_PTIMER_TIME_0);
    } while (hi != rd(NV_PTIMER_TIME_1));
    return ((uint64_t)hi << 32) | lo;
}

static void udelay(uint32_t us) {
    uint64_t end = ptimer_ns() + (uint64_t)us * 1000u;
    uint64_t guard = time_get_uptime_ms() + us / 1000u + 2;
    while (ptimer_ns() < end && time_get_uptime_ms() < guard) asm volatile("pause");
}

static bool rm_alloc(rm_mem_t* m, uint32_t bytes) {
    uint32_t pages = (bytes + 4095u) / 4096u;
    if (m->va && m->pages >= pages) {
        memset(m->va, 0, (size_t)m->pages * 4096u);
        return true;
    }
    m->va = (uint8_t*)pmm_try_alloc_pages_zeroed(pages);
    m->pages = m->va ? pages : 0;
    return m->va != NULL;
}

static bool fs_read(void* ctx, uint64_t offset, void* buf, uint32_t len) {
    minimafs_file_handle_t* h = (minimafs_file_handle_t*)ctx;
    if (offset > 0xFFFFFFFFull || !minimafs_seek(h, (uint32_t)offset)) return false;
    return minimafs_read(h, buf, len) == len;
}

static minimafs_file_handle_t* open_fw(const char* chip, const char* name, nv_fw_file_t* f) {
    char path[160];
    snprintf(path, sizeof(path), NV_FW_DIR "%s/%s", chip, name);
    minimafs_file_handle_t* h = minimafs_open(path, true);
    if (!h) {
        say("  missing %s\n", path);
        return NULL;
    }
    *f = (nv_fw_file_t){ fs_read, h, minimafs_size(h) };
    return h;
}

/* ----------------------------------------------------------- falcons */

// nouveau's nvkm_falcon_reset: disable (interfaces off, interrupts
// masked), engine reset (gp102_flcn_reset_eng), wait for IMEM/DMEM
// scrubbing, then enable, which writes the chip id into FALCON_RM.
// Signed ucode (booter_load) checks that register first and gives up
// early - error 0x31 - when it does not hold PMC_BOOT_0.
static bool falcon_reset(const nv_falcon_t* f) {
    fwr(f, NV_FALCON_ITFEN, frd(f, NV_FALCON_ITFEN) & ~3u);
    fwr(f, NV_FALCON_IRQMCLR, 0xFFFFFFFFu);
    fwr(f, NV_FALCON_ENGINE, frd(f, NV_FALCON_ENGINE) | 1u);
    udelay(10);
    fwr(f, NV_FALCON_ENGINE, frd(f, NV_FALCON_ENGINE) & ~1u);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = frd(f, NV_FALCON_DMACTL);
        if (!nv_pri_error(v) && !(v & 0x6u)) {
            fwr(f, NV_FALCON_RM, rd(NV_PMC_BOOT_0));
            return true;
        }
        udelay(10);
    }
    return false;
}

static bool riscv_active(void) {
    uint32_t v = frd(&k_gsp, NV_FALCON_RISCV_CORE_STATUS);
    return !nv_pri_error(v) && (v & 1u);
}

static bool wait_halt(const nv_falcon_t* f, uint32_t ms) {
    uint64_t end = time_get_uptime_ms() + ms;
    do {
        uint32_t c = frd(f, NV_FALCON_CPUCTL);
        if (!nv_pri_error(c) && (c & NV_FALCON_CPUCTL_HALTED)) return true;
        udelay(100);
    } while (time_get_uptime_ms() < end);
    return false;
}

/* ------------------------------------------------- firmware loading */

static bool load_booter(const char* chip) {
    nv_fw_file_t f;
    minimafs_file_handle_t* h = open_fw(chip, "gsp/booter_load-" NV_GSP_FW_VERSION ".bin", &f);
    if (!h) return false;
    const char* why = "";
    bool ok = nv_fw_parse_booter(&f, &g.booter, &why);
    if (ok) ok = rm_alloc(&g.booter_img, g.booter.data_size) || (why = "out of memory", false);
    if (ok) ok = f.read(f.ctx, g.booter.data_offset, g.booter_img.va, g.booter.data_size) || (why = "read error", false);
    // The production signature goes into its slot in the DMEM image.
    if (ok) ok = f.read(f.ctx, g.booter.sig_offset, g.booter_img.va + g.booter.patch_loc, g.booter.sig_size) ||
                 (why = "read error", false);
    minimafs_close(h);
    if (!ok) { say("  booter_load: %s\n", why); return false; }
    say("  booter_load: HS code 0x%x+0x%x, data 0x%x+0x%x, %u-byte signature patched at 0x%x\n",
        g.booter.app_offset, g.booter.app_size, g.booter.os_data_offset, g.booter.os_data_size,
        g.booter.sig_size, g.booter.patch_loc);
    return true;
}

static bool load_bootloader(const char* chip) {
    nv_fw_file_t f;
    minimafs_file_handle_t* h = open_fw(chip, "gsp/bootloader-" NV_GSP_FW_VERSION ".bin", &f);
    if (!h) return false;
    const char* why = "";
    bool ok = nv_fw_parse_rm_bootloader(&f, &g.bl, &why);
    if (ok) ok = rm_alloc(&g.boot, g.bl.data_size) || (why = "out of memory", false);
    if (ok) ok = f.read(f.ctx, g.bl.data_offset, g.boot.va, g.bl.data_size) || (why = "read error", false);
    minimafs_close(h);
    if (!ok) { say("  bootloader: %s\n", why); return false; }
    say("  RISC-V boot loader: %u bytes, descriptor v%u\n", g.bl.data_size, g.bl.version);
    return true;
}

// GSP-RM's image into single pages behind the radix3 table, and its
// signature into one contiguous buffer.
static bool load_gsp_image(const char* chip, uint16_t chipset) {
    uint32_t n = 0;
    const nv_fw_entry_t* list = nv_fw_manifest(chipset, &n);
    const nv_fw_entry_t* e = NULL;
    for (uint32_t i = 0; list && i < n; i++)
        if (list[i].kind == NV_FW_KIND_ELF) e = &list[i];
    if (!e) { say("  no GSP-RM image listed for this chip\n"); return false; }

    nv_fw_file_t f;
    minimafs_file_handle_t* h = open_fw(chip, e->file, &f);
    if (!h) return false;
    const char* why = "";
    uint64_t img_off, img_size, sig_off, sig_size;
    bool ok = nv_fw_elf_section(&f, e->sections[0], &img_off, &img_size, &why) &&
              nv_fw_elf_section(&f, e->sections[1], &sig_off, &sig_size, &why);
    if (ok && (img_size == 0 || img_size > (256ull << 20) || sig_size == 0 || sig_size > 0x10000)) {
        ok = false;
        why = "unexpected section sizes";
    }
    g.sig_size = (uint32_t)sig_size;
    if (ok) ok = rm_alloc(&g.sig, (uint32_t)sig_size) || (why = "out of memory", false);
    if (ok) ok = f.read(f.ctx, sig_off, g.sig.va, (uint32_t)sig_size) || (why = "read error", false);

    uint32_t pages = (uint32_t)((img_size + 4095u) / 4096u);
    if (ok && g.elf_page_count != pages) {
        if (g.elf_pages) { ok = false; why = "image size changed since the last boot attempt"; }
        else {
            rm_mem_t list_mem = { 0 };
            ok = rm_alloc(&list_mem, pages * (uint32_t)sizeof(uint8_t*)) || (why = "out of memory", false);
            g.elf_pages = (uint8_t**)list_mem.va;
            for (uint32_t i = 0; ok && i < pages; i++) {
                g.elf_pages[i] = (uint8_t*)pmm_try_alloc_pages_zeroed(1);
                if (!g.elf_pages[i]) { ok = false; why = "out of memory for the image"; }
            }
            if (ok) g.elf_page_count = pages;
        }
    }
    if (ok) {
        say("  GSP-RM image: %llu KiB, signature %llu bytes; loading ...\n",
            (unsigned long long)(img_size >> 10), (unsigned long long)sig_size);
        for (uint32_t i = 0; ok && i < pages; i++) {
            uint64_t at = (uint64_t)i * 4096u;
            uint32_t len = (img_size - at) < 4096u ? (uint32_t)(img_size - at) : 4096u;
            if (len < 4096u) memset(g.elf_pages[i] + len, 0, 4096u - len);
            ok = f.read(f.ctx, img_off + at, g.elf_pages[i], len) || (why = "read error", false);
        }
    }
    minimafs_close(h);
    if (!ok) { say("  GSP-RM image: %s\n", why); return false; }

    // radix3: level 2 lists the image pages, level 1 the level-2 pages,
    // level 0 the level-1 pages.
    for (int lvl = 2; lvl >= 0; lvl--) {
        uint32_t tpages = nv_radix3_pages(img_size, lvl);
        if (!rm_alloc(&g.rx3[lvl], tpages * 4096u)) { say("  out of memory for the page table\n"); return false; }
        uint64_t* pte = (uint64_t*)g.rx3[lvl].va;
        if (lvl == 2) {
            for (uint32_t i = 0; i < pages; i++) pte[i] = pa(g.elf_pages[i]);
        } else {
            for (uint32_t i = 0; i < g.rx3[lvl + 1].pages; i++) pte[i] = pa(g.rx3[lvl + 1].va) + i * 4096ull;
        }
    }
    g.layout.elf_size = img_size;
    return true;
}

/* -------------------------------------------------- sysmem structures */

static bool build_libos(void) {
    static const char* names[3] = { "LOGINIT", "LOGINTR", "LOGRM" };
    if (!rm_alloc(&g.libos, 4096)) return false;
    nv_libos_region_t* args = (nv_libos_region_t*)g.libos.va;
    for (int i = 0; i < 3; i++) {
        if (!rm_alloc(&g.log[i], NV_GSP_LOG_SIZE)) return false;
        // Each log buffer starts with a put pointer, then its own page list.
        uint64_t* pte = (uint64_t*)(g.log[i].va + 8);
        for (uint32_t p = 0; p < NV_GSP_LOG_SIZE / 4096u; p++) pte[p] = pa(g.log[i].va) + p * 4096ull;
        args[i] = (nv_libos_region_t){ nv_libos_id8(names[i]), pa(g.log[i].va), NV_GSP_LOG_SIZE,
                                       NV_LIBOS_KIND_CONTIGUOUS, NV_LIBOS_LOC_SYSMEM, { 0 } };
    }

    // Shared memory: page list, command queue, message queue.
    uint32_t qpages = 2u * NV_GSP_QUEUE_SIZE / NV_GSP_PAGE;
    uint32_t pte_count = qpages + (qpages * 8u + NV_GSP_PAGE - 1) / NV_GSP_PAGE;
    uint32_t pte_bytes = (pte_count * 8u + NV_GSP_PAGE - 1) & ~(NV_GSP_PAGE - 1);
    if (!rm_alloc(&g.shm, pte_bytes + 2u * NV_GSP_QUEUE_SIZE)) return false;
    uint64_t* pte = (uint64_t*)g.shm.va;
    for (uint32_t i = 0; i < pte_count; i++) pte[i] = pa(g.shm.va) + (uint64_t)i * NV_GSP_PAGE;
    g.cmdq = (volatile nv_msgq_hdr_t*)(g.shm.va + pte_bytes);
    g.msgq = (volatile nv_msgq_hdr_t*)(g.shm.va + pte_bytes + NV_GSP_QUEUE_SIZE);
    g.cmdq->tx.version = 0;
    g.cmdq->tx.size = NV_GSP_QUEUE_SIZE;
    g.cmdq->tx.entryOff = NV_GSP_PAGE;
    g.cmdq->tx.msgSize = NV_GSP_PAGE;
    g.cmdq->tx.msgCount = (NV_GSP_QUEUE_SIZE - NV_GSP_PAGE) / NV_GSP_PAGE;
    g.cmdq->tx.writePtr = 0;
    g.cmdq->tx.flags = 1;
    g.cmdq->tx.rxHdrOff = (uint32_t)offsetof(nv_msgq_hdr_t, readPtr);
    g.slots = g.cmdq->tx.msgCount;
    g.seq = 0;

    if (!rm_alloc(&g.rmargs, sizeof(nv_gsp_rmargs_t))) return false;
    nv_gsp_rmargs_t* ra = (nv_gsp_rmargs_t*)g.rmargs.va;
    ra->sharedMemPhysAddr = pa(g.shm.va);
    ra->pageTableEntryCount = pte_count;
    ra->cmdQueueOffset = pte_bytes;
    ra->statQueueOffset = pte_bytes + NV_GSP_QUEUE_SIZE;
    args[3] = (nv_libos_region_t){ nv_libos_id8("RMARGS"), pa(g.rmargs.va), sizeof(nv_gsp_rmargs_t),
                                   NV_LIBOS_KIND_CONTIGUOUS, NV_LIBOS_LOC_SYSMEM, { 0 } };

    return rm_alloc(&g.tx, RM_MAX_SLOTS * NV_GSP_PAGE) && rm_alloc(&g.rx, RM_MAX_SLOTS * NV_GSP_PAGE);
}

static bool build_meta(void) {
    if (!rm_alloc(&g.meta, sizeof(nv_gsp_wpr_meta_t))) return false;
    nv_gsp_wpr_meta_t* m = (nv_gsp_wpr_meta_t*)g.meta.va;
    nv_gsp_fill_wpr_meta(m, &g.layout);
    m->sysmemAddrOfRadix3Elf = pa(g.rx3[0].va);
    m->sysmemAddrOfBootloader = pa(g.boot.va);
    m->bootloaderCodeOffset = g.bl.code_offset;
    m->bootloaderDataOffset = g.bl.data_off;
    m->bootloaderManifestOffset = g.bl.manifest_offset;
    m->sysmemAddrOfSignature = pa(g.sig.va);
    m->sizeOfSignature = g.sig_size;
    return true;
}

/* ---------------------------------------------------- message queues */

static void fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static uint8_t* cmdq_slot(uint32_t i) { return (uint8_t*)g.cmdq + NV_GSP_PAGE + i * NV_GSP_PAGE; }
static uint8_t* msgq_slot(uint32_t i) { return (uint8_t*)g.msgq + NV_GSP_PAGE + i * NV_GSP_PAGE; }

static bool rpc_send(uint32_t function, const void* payload, uint32_t len) {
    uint32_t bytes = (uint32_t)(sizeof(nv_gsp_msg_t) + sizeof(nv_rpc_hdr_t)) + len;
    uint32_t slots = nv_gsp_msg_slots(bytes);
    if (slots > RM_MAX_SLOTS) return false;
    // GSP-RM's read position in our queue lives in its queue's header.
    uint32_t wp = g.cmdq->tx.writePtr, rp = g.msgq->readPtr;
    uint32_t used = (wp + g.slots - rp) % g.slots;
    if (g.slots - 1 - used < slots) return false;

    memset(g.tx.va, 0, slots * NV_GSP_PAGE);
    nv_gsp_msg_t* m = (nv_gsp_msg_t*)g.tx.va;
    nv_rpc_hdr_t* r = (nv_rpc_hdr_t*)(m + 1);
    r->header_version = NV_RPC_HEADER_VERSION;
    r->signature = NV_RPC_SIGNATURE;
    r->length = (uint32_t)sizeof(*r) + len;
    r->function = function;
    r->rpc_result = NV_RPC_RESULT_PENDING;
    r->rpc_result_private = NV_RPC_RESULT_PENDING;
    if (len) memcpy(r + 1, payload, len);
    m->seqNum = g.seq++;
    m->elemCount = slots;
    m->checksum = nv_gsp_checksum(m, (bytes + 7u) & ~7u);

    for (uint32_t i = 0; i < slots; i++)
        memcpy(cmdq_slot((wp + i) % g.slots), g.tx.va + i * NV_GSP_PAGE, NV_GSP_PAGE);
    fence();
    g.cmdq->tx.writePtr = (wp + slots) % g.slots;
    fence();
    fwr(&k_gsp, NV_FALCON_GSP_QUEUE_HEAD0, 0);      // doorbell
    return true;
}

static bool run_sequencer(const uint32_t* p, uint32_t dwords);

/* Takes one message off GSP-RM's queue into g.rx. Returns its RPC
 * header, or NULL when the queue is empty or the message is bad. */
static nv_rpc_hdr_t* rpc_recv(void) {
    uint32_t wp = g.msgq->tx.writePtr, rp = g.cmdq->readPtr;
    if (wp == rp || wp >= g.slots) return NULL;
    fence();
    nv_gsp_msg_t* m = (nv_gsp_msg_t*)msgq_slot(rp);
    uint32_t slots = m->elemCount;
    if (slots == 0 || slots > RM_MAX_SLOTS || slots > (wp + g.slots - rp) % g.slots) {
        say("  bad message from GSP-RM (%u slots); queue skipped\n", slots);
        g.cmdq->readPtr = wp;
        return NULL;
    }
    for (uint32_t i = 0; i < slots; i++)
        memcpy(g.rx.va + i * NV_GSP_PAGE, msgq_slot((rp + i) % g.slots), NV_GSP_PAGE);
    fence();
    g.cmdq->readPtr = (rp + slots) % g.slots;
    fence();
    return (nv_rpc_hdr_t*)(g.rx.va + sizeof(nv_gsp_msg_t));
}

// Handles events; returns the message if it is `want`, NULL otherwise.
static nv_rpc_hdr_t* handle(nv_rpc_hdr_t* r, uint32_t want) {
    if (r->function == want) return r;
    g.events++;
    uint32_t plen = r->length > sizeof(*r) ? r->length - (uint32_t)sizeof(*r) : 0;
    const uint8_t* payload = (const uint8_t*)(r + 1);
    switch (r->function) {
        case NV_RPC_EVENT_RUN_CPU_SEQUENCER:
            g.seq_runs++;
            if (!run_sequencer((const uint32_t*)payload, plen / 4)) g.seq_errors++;
            break;
        case NV_RPC_EVENT_OS_ERROR_LOG: {
            // exceptType, runlistId, chid, then text
            char text[96] = { 0 };
            for (uint32_t i = 0; i + 1 < sizeof(text) && 12 + i < plen && payload[12 + i]; i++)
                text[i] = (char)payload[12 + i];
            say("  GSP-RM error log: %s\n", text);
            break;
        }
        case NV_RPC_EVENT_UCODE_LIBOS_PRINT:
        case NV_RPC_EVENT_POST_EVENT:
            break;
        default:
            if (g.events <= 12)
                say("  event 0x%x %s (%u bytes)\n", r->function, nv_rpc_name(r->function), plen);
            break;
    }
    return NULL;
}

static nv_rpc_hdr_t* rpc_wait(uint32_t want, uint32_t ms) {
    uint64_t end = time_get_uptime_ms() + ms;
    do {
        nv_rpc_hdr_t* r = rpc_recv();
        if (r) {
            r = handle(r, want);
            if (r) return r;
            continue;
        }
        sleep(1);
    } while (time_get_uptime_ms() < end);
    return NULL;
}

/* -------------------------------------------------- booter on SEC2 */

// PIO load of booter_load (gm200_flcn_fw_load without a DMA context):
// non-secure code at IMEM 0, the HS application at its own offset
// (secure), data at DMEM 0. Then start at the non-secure code.
static bool run_booter(uint32_t mbox0, uint32_t mbox1, uint32_t* out0, uint32_t* out1) {
    if (!falcon_reset(&k_sec2)) { say("  SEC2: reset/scrub timeout\n"); return false; }
    fwr(&k_sec2, NV_FALCON_FBIF_CTL, frd(&k_sec2, NV_FALCON_FBIF_CTL) | 0x80u);
    fwr(&k_sec2, NV_FALCON_DMACTL, 0);
    const uint8_t* img = g.booter_img.va;
    if (g.booter.os_code_size)
        nv_falcon_imem_write(g.a, &k_sec2, g.booter.os_code_offset, img + g.booter.os_code_offset,
                             g.booter.os_code_size, g.booter.os_code_offset >> 8, false);
    nv_falcon_imem_write(g.a, &k_sec2, g.booter.app_offset, img + g.booter.app_offset,
                         g.booter.app_size, g.booter.app_offset >> 8, true);
    nv_falcon_dmem_write(g.a, &k_sec2, 0, img + g.booter.os_data_offset, g.booter.os_data_size);
    fwr(&k_sec2, NV_FALCON_MAILBOX0, mbox0);
    fwr(&k_sec2, NV_FALCON_MAILBOX1, mbox1);
    // Start in the non-secure stub (tu102_gsp_booter_ctor: boot_addr =
    // os_code_offset), which enters the signed application itself.
    // Starting at the application directly fails with 0x31.
    nv_falcon_start(g.a, &k_sec2, g.booter.os_code_offset);
    bool halted = wait_halt(&k_sec2, 5000);
    *out0 = frd(&k_sec2, NV_FALCON_MAILBOX0);
    *out1 = frd(&k_sec2, NV_FALCON_MAILBOX1);
    if (!halted) say("  SEC2: booter_load did not halt within 5 s\n");
    return halted;
}

static void gsp_prepare(void) {
    falcon_reset(&k_gsp);
    fwr(&k_gsp, NV_FALCON_FBIF_CTL, frd(&k_gsp, NV_FALCON_FBIF_CTL) | 0x80u);
    fwr(&k_gsp, NV_FALCON_DMACTL, 0);
    fwr(&k_gsp, NV_FALCON_MAILBOX0, (uint32_t)pa(g.libos.va));
    fwr(&k_gsp, NV_FALCON_MAILBOX1, (uint32_t)(pa(g.libos.va) >> 32));
}

/* -------------------------------------------------- CPU sequencer */

static bool run_sequencer(const uint32_t* p, uint32_t dwords) {
    if (dwords < 10) return false;
    uint32_t used = p[1];
    uint32_t* save = (uint32_t*)&p[2];     // regSaveArea[8]
    const uint32_t* cmd = p + 10;
    if (used > dwords - 10) used = dwords - 10;
    for (uint32_t i = 0; i < used;) {
        uint32_t op = cmd[i];
        int n = nv_seq_payload_dwords(op);
        if (n < 0 || i + 1 + (uint32_t)n > used) {
            say("  sequencer: bad opcode %u\n", op);
            return false;
        }
        const uint32_t* a = cmd + i + 1;
        i += 1 + (uint32_t)n;
        switch (op) {
            case NV_SEQ_REG_WRITE:  wr(a[0], a[1]); break;
            case NV_SEQ_REG_MODIFY: mask(a[0], a[1], a[2]); break;
            case NV_SEQ_REG_POLL: {
                uint32_t timeout = a[3] ? a[3] : 4000000u;
                uint64_t end = ptimer_ns() + (uint64_t)timeout * 1000u;
                uint64_t guard = time_get_uptime_ms() + timeout / 1000u + 2;
                while ((rd(a[0]) & a[1]) != a[2]) {
                    if (ptimer_ns() > end || time_get_uptime_ms() > guard) {
                        say("  sequencer: poll 0x%x timed out\n", a[0]);
                        return false;
                    }
                    udelay(10);
                }
                break;
            }
            case NV_SEQ_DELAY_US:   udelay(a[0]); break;
            case NV_SEQ_REG_STORE:  if (a[1] < 8) save[a[1]] = rd(a[0]); break;
            case NV_SEQ_CORE_RESET:
                falcon_reset(&k_gsp);
                fwr(&k_gsp, NV_FALCON_FBIF_CTL, frd(&k_gsp, NV_FALCON_FBIF_CTL) | 0x80u);
                fwr(&k_gsp, NV_FALCON_DMACTL, 0);
                break;
            case NV_SEQ_CORE_START:
                if (frd(&k_gsp, NV_FALCON_CPUCTL) & NV_FALCON_CPUCTL_ALIAS_EN)
                    fwr(&k_gsp, NV_FALCON_CPUCTL_ALIAS, NV_FALCON_CPUCTL_STARTCPU);
                else
                    fwr(&k_gsp, NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
                break;
            case NV_SEQ_CORE_WAIT_HALT:
                if (!wait_halt(&k_gsp, 2000)) { say("  sequencer: GSP did not halt\n"); return false; }
                break;
            case NV_SEQ_CORE_RESUME: {
                // Restart GSP-RM through booter (nouveau r535 CORE_RESUME).
                gsp_prepare();
                fwr(&k_sec2, NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
                uint64_t end = time_get_uptime_ms() + 2000;
                while (!(rd(0x1180F8) & 0x04000000u) && time_get_uptime_ms() < end) udelay(100);
                uint32_t mb = frd(&k_sec2, NV_FALCON_MAILBOX0);
                if (mb) { say("  sequencer: resume via SEC2 failed, mailbox 0x%x\n", mb); return false; }
                fwr(&k_gsp, NV_FALCON_OS, g.bl.app_version);
                if (!riscv_active()) { say("  sequencer: GSP RISC-V not running after resume\n"); return false; }
                break;
            }
        }
    }
    return true;
}

/* ------------------------------------------------- failure details */

// GSP-RM's LibOS log buffers (LOGINIT, LOGINTR, LOGRM): word 0 is the
// put pointer (entries written, in 8-byte words), then the page list,
// then the entries. Entries are binary (format strings live in a debug
// file NVIDIA does not ship), so they go out as hex: on a netbooted
// machine they arrive in build/netboot/logs/, where
// tools/gpu/gsp_log_strings.py looks up any string the values point at.
static void dump_gsp_logs(void) {
    static const char* names[3] = { "LOGINIT", "LOGINTR", "LOGRM" };
    uint32_t pte_words = NV_GSP_LOG_SIZE / 4096u;
    uint32_t max_words = NV_GSP_LOG_SIZE / 8u - 1u - pte_words;
    for (int i = 0; i < 3; i++) {
        volatile uint64_t* buf = (volatile uint64_t*)g.log[i].va;
        uint64_t put = buf[0];
        say("  %s: %llu words\n", names[i], (unsigned long long)put);
        if (!put) continue;
        uint32_t n = put < max_words ? (uint32_t)put : max_words;
        if (n > 512) n = 512;
        const volatile uint64_t* entries = buf + 1 + pte_words;
        char line[160];
        for (uint32_t w = 0; w < n; w += 4) {
            int used = snprintf(line, sizeof(line), "gsplog %s %04u:", names[i], w);
            for (uint32_t k = w; k < n && k < w + 4; k++)
                used += snprintf(line + used, sizeof(line) - (size_t)used, " %016llx",
                                 (unsigned long long)entries[k]);
            snprintf(line + used, sizeof(line) - (size_t)used, "\n");
            serial_write_str(line);
            if (w < 32) say("  %s", line + 7);       // the first few on screen too
        }
    }
    say("  GSP: mailbox 0x%x 0x%x, RISC-V %s, cpuctl 0x%x; queues: ours write %u read %u, GSP's write %u\n",
        frd(&k_gsp, NV_FALCON_MAILBOX0), frd(&k_gsp, NV_FALCON_MAILBOX1),
        riscv_active() ? "active" : "stopped", frd(&k_gsp, NV_FALCON_CPUCTL),
        g.cmdq->tx.writePtr, g.msgq->readPtr, g.msgq->tx.writePtr);
}

/* -------------------------------------------------------- the RPCs */

static void queue_boot_rpcs(void) {
    static uint8_t buf[NV_GSP_SYSINFO_BYTES];
    memset(buf, 0, sizeof(buf));
    nv_gsp_sysinfo_t* si = (nv_gsp_sysinfo_t*)buf;
    pci_device_t* p = g.a->pci;
    si->gpuPhysAddr = p->bar_address[0];
    si->gpuPhysFbAddr = p->bar_address[1];
    si->gpuPhysInstAddr = p->bar_address[3];
    si->nvDomainBusDeviceFunc = ((uint64_t)p->bus << 8) | ((uint64_t)p->device << 3) | p->function;
    si->maxUserVa = 0x00007FFFFFFFF000ull;
    si->pciConfigMirrorBase = 0x088000;
    si->pciConfigMirrorSize = 0x001000;
    bool ok = rpc_send(NV_RPC_GSP_SET_SYSTEM_INFO, buf, sizeof(buf));

    static const nv_reg_entry_t reg[] = {
        { "RMSecBusResetEnable", 1 },
        { "RMForcePcieConfigSave", 1 },
    };
    uint32_t n = nv_gsp_pack_registry(buf, sizeof(buf), reg, 2);
    ok = ok && n && rpc_send(NV_RPC_SET_REGISTRY, buf, n);
    say("  queued GSP_SET_SYSTEM_INFO and SET_REGISTRY%s\n", ok ? "" : " (FAILED)");
}

// Step 3a: GET_GSP_STATIC_INFO. The reply holds the GPU's name
// ("NVIDIA GeForce ..."); finding it shows the RPC path works.
static void first_rpc(void) {
    static uint8_t req[NV_GSP_SYSINFO_BYTES];
    memset(req, 0, sizeof(req));
    say("\nStep 3a: first RPC (GET_GSP_STATIC_INFO)\n");
    if (!rpc_send(NV_RPC_GET_GSP_STATIC_INFO, req, sizeof(req))) { say("  command queue full\n"); return; }
    nv_rpc_hdr_t* r = rpc_wait(NV_RPC_GET_GSP_STATIC_INFO, 3000);
    if (!r) { say("  no reply within 3 s\n"); return; }
    say("  reply: result 0x%x, %u bytes\n", r->rpc_result, r->length);
    g.rpc_ok = r->rpc_result == 0;
    const char* s = (const char*)(r + 1);
    uint32_t len = r->length > sizeof(*r) ? r->length - (uint32_t)sizeof(*r) : 0;
    for (uint32_t i = 0; i + 6 < len; i++) {
        if (memcmp(s + i, "NVIDIA", 6) != 0) continue;
        uint32_t k = 0;
        while (k + 1 < sizeof(g.gpu_name) && i + k < len && s[i + k] >= 32 && s[i + k] < 127) {
            g.gpu_name[k] = s[i + k];
            k++;
        }
        g.gpu_name[k] = 0;
        break;
    }
    if (g.gpu_name[0]) say("  GSP-RM says this is: %s\n", g.gpu_name);
    say(g.rpc_ok ? "  ok: RPC round trip works.\n" : "  GSP-RM answered with an error.\n");
}

/* ------------------------------------------------------------ boot */

bool nv_gsp_rm_boot(gpu_adapter_t* a, void (*print)(const char* text)) {
    g.print = print;
    g.log_len = 0;
    if (g.running && g.a == a) {
        say("GSP-RM is already running.\n");
        if (!g.rpc_ok) first_rpc();
        return true;
    }
    g.a = a;
    g.events = g.seq_runs = g.seq_errors = 0;
    uint16_t chipset = nv_chipset(a);
    const char* chip = nv_chip_name(chipset);
    say("\nGSP boot, step 2b: booter_load on SEC2, GSP-RM\n");
    if (!chip) { say("  no firmware list for chipset 0x%x\n", chipset); return false; }

    uint64_t wlo = 0, whi = 0;
    if (!nv_gsp_wpr2(a, &wlo, &whi)) { say("  WPR2 is not set up: FWSEC-FRTS has to run first\n"); return false; }
    if (frd(&k_sec2, NV_FALCON_SEC2_DEBUG) & (1u << 20)) {
        say("  SEC2 is debug-fused: the production signature will not verify\n");
        return false;
    }

    if (!load_booter(chip) || !load_bootloader(chip) || !load_gsp_image(chip, chipset)) {
        save_log();
        return false;
    }

    uint64_t fb = nv_vidmem_size(rd(NV_PFB_VIDMEM_SIZE));
    uint64_t vga = nv_gsp_vga_workspace(rd(NV_PDISP_VGA_WORKSPACE), fb);
    nv_gsp_layout(&g.layout, fb, vga, g.bl.data_size, g.layout.elf_size);
    const nv_gsp_layout_t* l = &g.layout;
    say("  VRAM layout: WPR2 0x%llx-0x%llx, heap 0x%llx (%llu MiB), image 0x%llx, boot 0x%llx\n",
        (unsigned long long)l->wpr2_addr, (unsigned long long)l->wpr2_end,
        (unsigned long long)l->heap_addr, (unsigned long long)(l->heap_size >> 20),
        (unsigned long long)l->elf_addr, (unsigned long long)l->boot_addr);
    if (l->frts_addr != wlo) {
        say("  FRTS at 0x%llx, but WPR2 starts at 0x%llx: layouts disagree, not booting\n",
            (unsigned long long)l->frts_addr, (unsigned long long)wlo);
        save_log();
        return false;
    }

    if (!build_libos() || !build_meta()) { say("  out of memory\n"); save_log(); return false; }
    say("  sysmem: WPR meta 0x%llx, LibOS args 0x%llx, queues 0x%llx (%u slots each)\n",
        (unsigned long long)pa(g.meta.va), (unsigned long long)pa(g.libos.va),
        (unsigned long long)pa(g.shm.va), g.slots);
    queue_boot_rpcs();

    pci_enable_io_busmaster(a->pci);       // GSP-RM reads sysmem
    gsp_prepare();
    say("  Starting booter_load on SEC2 ...\n");
    save_log();                            // in case the screen goes dark from here on

    uint32_t mb0 = 0, mb1 = 0;
    bool halted = run_booter((uint32_t)pa(g.meta.va), (uint32_t)(pa(g.meta.va) >> 32), &mb0, &mb1);
    say("  booter_load %s, mailbox 0x%x 0x%x\n", halted ? "halted" : "still running", mb0, mb1);
    if (!halted || mb0 != 0) {
        say("  booter_load failed (mailbox 0 is its error code)\n");
        save_log();
        return false;
    }
    fwr(&k_gsp, NV_FALCON_OS, g.bl.app_version);
    nv_gsp_wpr_meta_t* m = (nv_gsp_wpr_meta_t*)g.meta.va;
    say("  GSP RISC-V %s, WPR meta %s\n", riscv_active() ? "running" : "NOT running",
        m->verified == NV_GSP_WPR_META_VERIFIED ? "verified by booter" : "not marked verified");

    say("  Waiting for GSP-RM (GSP_INIT_DONE) ...\n");
    save_log();
    nv_rpc_hdr_t* r = rpc_wait(NV_RPC_EVENT_GSP_INIT_DONE, 15000);
    uint64_t* put = (uint64_t*)g.log[0].va;
    say("  %u events, %u sequencer runs (%u failed), init log put pointer %llu\n",
        g.events, g.seq_runs, g.seq_errors, (unsigned long long)put[0]);
    if (!r) {
        say("  GSP-RM did not report GSP_INIT_DONE within 15 s. GSP mailbox 0x%x 0x%x\n",
            frd(&k_gsp, NV_FALCON_MAILBOX0), frd(&k_gsp, NV_FALCON_MAILBOX1));
        dump_gsp_logs();
        save_log();
        return false;
    }
    g.running = true;
    say("  ok: GSP-RM is running.\n");
    first_rpc();
    say("\nNext: step 3b, RM objects and a channel (not written yet).\n");
    save_log();
    return true;
}

bool nv_gsp_rm_running(gpu_adapter_t* a) { return g.running && g.a == a; }
bool nv_gsp_rm_rpc_ok(gpu_adapter_t* a) { return g.running && g.a == a && g.rpc_ok; }
