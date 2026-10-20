#include <stdint.h>
#include <stdbool.h>
#include "x86_64/blockdev.h"
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/pmm.h"
#include "x86_64/spinlock.h"
#include "serial.h"
#include "string.h"

/*
 * NVMe driver (polling, one I/O queue per controller, namespace 1..N).
 *
 * Memory is identity mapped, so kernel buffers are their own physical
 * addresses; transfers use PRP lists for anything over two pages.
 * Every namespace becomes a block device: nvme0, nvme1, ...
 */

#define NVME_REG_CAP   0x00
#define NVME_REG_VS    0x08
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30

#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY  0x06
#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ  0x02
#define NVME_CMD_FLUSH 0x00

#define QUEUE_ENTRIES 64u
#define MAX_TRANSFER_PAGES 64u      // 256 KiB per command
#define MAX_CONTROLLERS 4
#define MAX_NAMESPACES 8

typedef struct {
    uint32_t cdw0;          // opcode | cid << 16
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;        // bit 0 = phase
} __attribute__((packed)) nvme_cqe_t;

typedef struct {
    nvme_sqe_t* sq;
    volatile nvme_cqe_t* cq;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t phase;
    uint16_t qid;
    uint16_t next_cid;
} nvme_queue_t;

typedef struct {
    volatile uint8_t* regs;
    uint32_t doorbell_stride;
    uint32_t timeout_ms;
    nvme_queue_t admin;
    nvme_queue_t io;
    uint64_t* prp_list;     // one page, for the I/O queue
    spinlock_t lock;
} nvme_ctrl_t;

typedef struct {
    nvme_ctrl_t* ctrl;
    uint32_t nsid;
    uint32_t sector_size;
} nvme_ns_t;

static nvme_ctrl_t g_ctrls[MAX_CONTROLLERS];
static nvme_ns_t g_ns[MAX_CONTROLLERS * MAX_NAMESPACES];
static uint32_t g_ns_count;

static inline uint32_t rd32(nvme_ctrl_t* c, uint32_t reg) {
    return *(volatile uint32_t*)(c->regs + reg);
}
static inline uint64_t rd64(nvme_ctrl_t* c, uint32_t reg) {
    return (uint64_t)rd32(c, reg) | ((uint64_t)rd32(c, reg + 4) << 32);
}
static inline void wr32(nvme_ctrl_t* c, uint32_t reg, uint32_t v) {
    *(volatile uint32_t*)(c->regs + reg) = v;
}
static inline void wr64(nvme_ctrl_t* c, uint32_t reg, uint64_t v) {
    wr32(c, reg, (uint32_t)v);
    wr32(c, reg + 4, (uint32_t)(v >> 32));
}

static void delay_us(uint32_t us) {
    // ~1 us per port 0x80 write on PC hardware.
    for (uint32_t i = 0; i < us; i++) __asm__ volatile("outb %%al, $0x80" ::: "memory");
}

static bool wait_ready(nvme_ctrl_t* c, bool ready) {
    for (uint32_t ms = 0; ms < c->timeout_ms; ms++) {
        uint32_t csts = rd32(c, NVME_REG_CSTS);
        if (csts == 0xFFFFFFFFu) return false;
        if (((csts & 1u) != 0) == ready) return true;
        delay_us(1000);
    }
    return false;
}

static void ring_sq(nvme_ctrl_t* c, nvme_queue_t* q) {
    uint32_t off = 0x1000 + (2u * q->qid) * c->doorbell_stride;
    wr32(c, off, q->sq_tail);
}

static void ring_cq(nvme_ctrl_t* c, nvme_queue_t* q) {
    uint32_t off = 0x1000 + (2u * q->qid + 1u) * c->doorbell_stride;
    wr32(c, off, q->cq_head);
}

// Submits one command and polls for its completion. Returns the status
// field (0 = success) or 0xFFFF on timeout.
static uint16_t submit(nvme_ctrl_t* c, nvme_queue_t* q, nvme_sqe_t* cmd) {
    uint16_t cid = q->next_cid++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % QUEUE_ENTRIES);
    __asm__ volatile("mfence" ::: "memory");
    ring_sq(c, q);

    for (uint64_t spin = 0; spin < 2000000000ull; spin++) {
        volatile nvme_cqe_t* e = &q->cq[q->cq_head];
        if ((e->status & 1u) == q->phase) {
            uint16_t status = (uint16_t)(e->status >> 1);
            uint16_t got = e->cid;
            q->cq_head = (uint16_t)((q->cq_head + 1) % QUEUE_ENTRIES);
            if (q->cq_head == 0) q->phase ^= 1u;
            ring_cq(c, q);
            if (got == cid) return status;
        }
        __asm__ volatile("pause");
    }
    serial_write_str("nvme: command timeout\n");
    return 0xFFFF;
}

static bool queue_alloc(nvme_queue_t* q, uint16_t qid) {
    memset(q, 0, sizeof(*q));
    q->sq = (nvme_sqe_t*)alloc_page_zeroed();     // 64 * 64 bytes = one page
    q->cq = (volatile nvme_cqe_t*)alloc_page_zeroed();
    q->qid = qid;
    q->phase = 1;
    return q->sq && q->cq;
}

// Fills prp1/prp2 for a physically contiguous buffer.
static bool build_prps(nvme_ctrl_t* c, uint64_t addr, uint64_t bytes, nvme_sqe_t* cmd) {
    cmd->prp1 = addr;
    uint64_t first = 4096 - (addr & 4095);
    if (bytes <= first) { cmd->prp2 = 0; return true; }
    uint64_t rest = bytes - first;
    uint64_t next = (addr + first);
    if (rest <= 4096) { cmd->prp2 = next; return true; }
    uint64_t pages = (rest + 4095) / 4096;
    if (pages > 512) return false;
    for (uint64_t i = 0; i < pages; i++) c->prp_list[i] = next + i * 4096;
    cmd->prp2 = (uint64_t)(uintptr_t)c->prp_list;
    return true;
}

static bool nvme_io(void* ctx, uint64_t lba, uint32_t count, void* buffer, bool write) {
    nvme_ns_t* ns = (nvme_ns_t*)ctx;
    nvme_ctrl_t* c = ns->ctrl;
    uint64_t flags = spinlock_acquire(&c->lock);
    bool ok = true;
    uint8_t* p = (uint8_t*)buffer;
    uint32_t max_blocks = (MAX_TRANSFER_PAGES * 4096u) / ns->sector_size;
    while (count && ok) {
        uint32_t n = count < max_blocks ? count : max_blocks;
        nvme_sqe_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0 = write ? NVME_CMD_WRITE : NVME_CMD_READ;
        cmd.nsid = ns->nsid;
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = n - 1;
        ok = build_prps(c, (uint64_t)(uintptr_t)p, (uint64_t)n * ns->sector_size, &cmd) &&
             submit(c, &c->io, &cmd) == 0;
        lba += n;
        count -= n;
        p += (uint64_t)n * ns->sector_size;
    }
    spinlock_release(&c->lock, flags);
    if (!ok) serial_write_str("nvme: I/O error\n");
    return ok;
}

static bool nvme_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    return nvme_io(ctx, lba, count, buffer, false);
}

static bool nvme_write(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    return nvme_io(ctx, lba, count, buffer, true);
}

static uint32_t init_controller(pci_device_t* pci, nvme_ctrl_t* c) {
    memset(c, 0, sizeof(*c));
    if (pci->bar_type[0] != PCI_BAR_MEM || !pci->bar_address[0]) return 0;
    pci_enable_io_busmaster(pci);
    uint64_t len = pci->bar_length[0] ? pci->bar_length[0] : 0x4000;
    if (len > 0x10000) len = 0x10000;
    void* va = mmio_alloc_va(len);
    if (!va || !map_mmio_page((uintptr_t)va, pci->bar_address[0], len)) return 0;
    c->regs = (volatile uint8_t*)va;

    uint64_t cap = rd64(c, NVME_REG_CAP);
    if (cap == ~0ull) return 0;
    c->doorbell_stride = 4u << ((cap >> 32) & 0xF);
    c->timeout_ms = (uint32_t)(((cap >> 24) & 0xFF) * 500) + 1000;
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
    if (mqes < QUEUE_ENTRIES) {
        serial_write_str("nvme: queues too small\n");
        return 0;
    }

    // Reset, then bring up the admin queue.
    wr32(c, NVME_REG_CC, rd32(c, NVME_REG_CC) & ~1u);
    if (!wait_ready(c, false)) { serial_write_str("nvme: disable timeout\n"); return 0; }
    if (!queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1)) return 0;
    c->prp_list = (uint64_t*)alloc_page_zeroed();
    if (!c->prp_list) return 0;
    wr32(c, NVME_REG_AQA, ((QUEUE_ENTRIES - 1) << 16) | (QUEUE_ENTRIES - 1));
    wr64(c, NVME_REG_ASQ, (uint64_t)(uintptr_t)c->admin.sq);
    wr64(c, NVME_REG_ACQ, (uint64_t)(uintptr_t)c->admin.cq);
    // 64-byte SQ entries, 16-byte CQ entries, 4 KiB pages, NVM command set.
    wr32(c, NVME_REG_CC, (4u << 20) | (6u << 16) | 1u);
    if (!wait_ready(c, true)) { serial_write_str("nvme: enable timeout\n"); return 0; }

    uint8_t* id = (uint8_t*)alloc_page_zeroed();
    if (!id) return 0;
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.prp1 = (uint64_t)(uintptr_t)id;
    cmd.cdw10 = 1;                                  // controller
    if (submit(c, &c->admin, &cmd) != 0) { serial_write_str("nvme: identify failed\n"); return 0; }
    char model[41];
    memcpy(model, id + 24, 40);
    model[40] = '\0';
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == '\0'); i--) model[i] = '\0';
    uint32_t namespaces = *(uint32_t*)(id + 516);

    // I/O completion queue 1, then submission queue 1 (polled, no IRQ).
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_CREATE_CQ;
    cmd.prp1 = (uint64_t)(uintptr_t)c->io.cq;
    cmd.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | 1;
    cmd.cdw11 = 1;                                  // physically contiguous
    if (submit(c, &c->admin, &cmd) != 0) { serial_write_str("nvme: create CQ failed\n"); return 0; }
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_CREATE_SQ;
    cmd.prp1 = (uint64_t)(uintptr_t)c->io.sq;
    cmd.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | 1;
    cmd.cdw11 = (1u << 16) | 1u;                    // CQ 1, contiguous
    if (submit(c, &c->admin, &cmd) != 0) { serial_write_str("nvme: create SQ failed\n"); return 0; }

    uint32_t found = 0;
    if (namespaces > MAX_NAMESPACES) namespaces = MAX_NAMESPACES;
    for (uint32_t nsid = 1; nsid <= namespaces && g_ns_count < MAX_CONTROLLERS * MAX_NAMESPACES; nsid++) {
        memset(id, 0, 4096);
        memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0 = NVME_ADMIN_IDENTIFY;
        cmd.nsid = nsid;
        cmd.prp1 = (uint64_t)(uintptr_t)id;
        cmd.cdw10 = 0;                              // namespace
        if (submit(c, &c->admin, &cmd) != 0) continue;
        uint64_t blocks = *(uint64_t*)(id + 0);
        uint8_t flbas = id[26] & 0xF;
        uint32_t lbaf = *(uint32_t*)(id + 128 + flbas * 4);
        uint32_t lbads = (lbaf >> 16) & 0xFF;
        if (!blocks || lbads < 9 || lbads > 12) continue;
        nvme_ns_t* ns = &g_ns[g_ns_count++];
        ns->ctrl = c;
        ns->nsid = nsid;
        ns->sector_size = 1u << lbads;
        uint32_t max_sectors = (MAX_TRANSFER_PAGES * 4096u) / ns->sector_size;
        if (blockdev_register_disk("nvme", model, ns->sector_size, blocks, ns,
                                   nvme_read, nvme_write, max_sectors)) {
            found++;
        }
    }
    free_page(id);
    return found;
}

uint32_t nvme_probe_all(void) {
    extern pci_device_t pci_devices[];
    extern int pci_device_count;
    uint32_t controllers = 0, disks = 0;
    for (int i = 0; i < pci_device_count && controllers < MAX_CONTROLLERS; i++) {
        pci_device_t* pci = &pci_devices[i];
        if (pci->class_code != 0x01 || pci->subclass != 0x08 || pci->prog_if != 0x02) continue;
        uint32_t n = init_controller(pci, &g_ctrls[controllers]);
        if (n) {
            controllers++;
            disks += n;
        }
    }
    return disks;
}
