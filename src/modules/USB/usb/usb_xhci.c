#include <stdint.h>
#include <stdbool.h>
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/pmm.h"
#include "keyboard/usbkeyboard.h"
#include "mouse/usbmouse.h"
#include "serial.h"
#include "string.h"

/*
 * xHCI (USB 3) host controller driver: HID boot keyboards and mice.
 *
 * Every PC from the last decade has only xHCI controllers, so without this
 * a USB keyboard works only through the firmware's PS/2 emulation (if at
 * all) and a USB mouse not at all.
 *
 * Scope: devices plugged straight into a root port (no hubs), boot
 * protocol keyboards and mice, polled from the timer interrupt
 * (xhci_poll). Reports go to the same keyboard and mouse code as UHCI's.
 * Memory is identity mapped, so physical = virtual for every buffer.
 */

#define TRB_NORMAL        1u
#define TRB_SETUP         2u
#define TRB_DATA          3u
#define TRB_STATUS        4u
#define TRB_LINK          6u
#define TRB_ENABLE_SLOT   9u
#define TRB_ADDRESS_DEV   11u
#define TRB_CONFIG_EP     12u
#define TRB_EVALUATE_CTX  13u
#define TRB_TRANSFER_EVT  32u
#define TRB_COMMAND_EVT   33u

#define CC_SUCCESS        1u
#define CC_SHORT_PACKET   13u

#define RING_TRBS 256u
#define MAX_CONTROLLERS 4
#define MAX_DEVICES 8

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) trb_t;

typedef struct {
    trb_t* trbs;
    uint32_t index;
    uint32_t cycle;
} ring_t;

typedef struct xhci xhci_t;

typedef struct {
    bool used;
    xhci_t* hc;
    uint8_t slot;
    uint8_t port;
    uint8_t speed;
    uint8_t dci;            // interrupt IN endpoint's context index
    bool keyboard;
    ring_t ep0;
    ring_t intr;
    uint8_t* out_ctx;
    uint8_t* in_ctx;
    uint8_t* report;        // one page; report at offset 0
    uint16_t report_size;
} xhci_dev_t;

struct xhci {
    bool ready;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* rt;
    volatile uint32_t* db;
    uint32_t ctx_size;
    uint32_t max_slots;
    uint32_t max_ports;
    uint64_t* dcbaa;
    ring_t cmd;
    trb_t* events;
    uint32_t ev_index;
    uint32_t ev_cycle;
    // Last command completion seen (for synchronous commands).
    volatile bool cmd_done;
    uint32_t cmd_code;
    uint32_t cmd_slot;
    // Last transfer event on EP0 (synchronous control transfers).
    volatile bool xfer_done;
    uint32_t xfer_code;
};

static xhci_t g_hcs[MAX_CONTROLLERS];
static uint32_t g_hc_count;
static xhci_dev_t g_devs[MAX_DEVICES];
static volatile bool g_polling_on;

/* ------------------------------------------------------------- helpers */

static inline uint32_t r32(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}
static inline void w32(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}
static inline void w64(volatile uint8_t* base, uint32_t off, uint64_t v) {
    w32(base, off, (uint32_t)v);
    w32(base, off + 4, (uint32_t)(v >> 32));
}

static void udelay(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) __asm__ volatile("outb %%al, $0x80" ::: "memory");
}

static bool ring_init(ring_t* r) {
    r->trbs = (trb_t*)alloc_page_zeroed();     // 256 TRBs * 16 bytes = 4 KiB
    if (!r->trbs) return false;
    r->index = 0;
    r->cycle = 1;
    trb_t* link = &r->trbs[RING_TRBS - 1];
    link->param = (uint64_t)(uintptr_t)r->trbs;
    link->control = (TRB_LINK << 10) | (1u << 1);  // toggle cycle
    return true;
}

static trb_t* ring_push(ring_t* r, uint64_t param, uint32_t status, uint32_t control) {
    trb_t* t = &r->trbs[r->index];
    t->param = param;
    t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = (control & ~1u) | r->cycle;
    r->index++;
    if (r->index == RING_TRBS - 1) {               // reached the link TRB
        trb_t* link = &r->trbs[RING_TRBS - 1];
        link->control = (link->control & ~1u) | r->cycle;
        r->cycle ^= 1u;
        r->index = 0;
    }
    return t;
}

static volatile uint8_t* port_reg(xhci_t* hc, uint32_t port) {
    return hc->op + 0x400 + 0x10 * (port - 1);
}

static uint32_t* ctx_at(xhci_t* hc, uint8_t* base, uint32_t index) {
    return (uint32_t*)(base + (size_t)index * hc->ctx_size);
}

/* --------------------------------------------------------------- events */

static void handle_transfer(xhci_t* hc, trb_t* ev);

// Processes every pending event. Returns true if any was handled.
static bool process_events(xhci_t* hc) {
    bool any = false;
    for (;;) {
        trb_t* ev = &hc->events[hc->ev_index];
        if ((ev->control & 1u) != hc->ev_cycle) break;
        uint32_t type = (ev->control >> 10) & 0x3F;
        if (type == TRB_COMMAND_EVT) {
            hc->cmd_code = ev->status >> 24;
            hc->cmd_slot = ev->control >> 24;
            hc->cmd_done = true;
        } else if (type == TRB_TRANSFER_EVT) {
            handle_transfer(hc, ev);
        }
        any = true;
        hc->ev_index++;
        if (hc->ev_index == RING_TRBS) {
            hc->ev_index = 0;
            hc->ev_cycle ^= 1u;
        }
    }
    if (any) {
        uint64_t erdp = (uint64_t)(uintptr_t)&hc->events[hc->ev_index];
        w64(hc->rt, 0x20 + 0x18, erdp | (1u << 3));    // clear busy
    }
    return any;
}

static bool command(xhci_t* hc, uint64_t param, uint32_t control, uint32_t* slot_out) {
    hc->cmd_done = false;
    ring_push(&hc->cmd, param, 0, control);
    hc->db[0] = 0;
    for (uint32_t i = 0; i < 2000000 && !hc->cmd_done; i++) {
        process_events(hc);
        if (!hc->cmd_done) udelay(1);
    }
    if (!hc->cmd_done) {
        serial_write_str("xhci: command timeout\n");
        return false;
    }
    if (slot_out) *slot_out = hc->cmd_slot;
    return hc->cmd_code == CC_SUCCESS;
}

/* ------------------------------------------------------ control requests */

static bool control(xhci_dev_t* d, uint8_t req_type, uint8_t request, uint16_t value,
                    uint16_t index, uint16_t length, void* data) {
    xhci_t* hc = d->hc;
    uint64_t setup = (uint64_t)req_type | ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);
    bool in = (req_type & 0x80) != 0;
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    ring_push(&d->ep0, setup, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (length) {
        ring_push(&d->ep0, (uint64_t)(uintptr_t)data, length,
                  (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    }
    // Status stage goes the other way (IN when there is no data).
    uint32_t status_in = (!length || !in) ? (1u << 16) : 0;
    hc->xfer_done = false;
    ring_push(&d->ep0, 0, 0, (TRB_STATUS << 10) | status_in | (1u << 5));
    hc->db[d->slot] = 1;                            // EP0
    for (uint32_t i = 0; i < 2000000 && !hc->xfer_done; i++) {
        process_events(hc);
        if (!hc->xfer_done) udelay(1);
    }
    return hc->xfer_done &&
           (hc->xfer_code == CC_SUCCESS || hc->xfer_code == CC_SHORT_PACKET);
}

static void queue_report(xhci_dev_t* d) {
    memset(d->report, 0, 64);
    ring_push(&d->intr, (uint64_t)(uintptr_t)d->report, d->report_size,
              (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));  // IOC, ISP
    d->hc->db[d->slot] = d->dci;
}

static void handle_transfer(xhci_t* hc, trb_t* ev) {
    uint32_t slot = ev->control >> 24;
    uint32_t ep = (ev->control >> 16) & 0x1F;
    uint32_t code = ev->status >> 24;
    if (ep == 1) {                                   // EP0: a control transfer
        hc->xfer_code = code;
        hc->xfer_done = true;
        return;
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        xhci_dev_t* d = &g_devs[i];
        if (!d->used || d->hc != hc || d->slot != slot || d->dci != ep) continue;
        if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
            if (d->keyboard) usb_keyboard_process_report((const usb_hid_keyboard_report_t*)d->report);
            else usb_mouse_process_report((const usb_hid_mouse_report_t*)d->report);
        }
        queue_report(d);
        return;
    }
}

/* ----------------------------------------------------------- enumeration */

static uint32_t ep0_packet_size(uint8_t speed) {
    switch (speed) {
        case 2: return 8;       // low speed
        case 1: return 8;       // full speed: 8 until the descriptor says more
        case 3: return 64;      // high speed
        default: return 512;    // super speed
    }
}

static bool setup_device(xhci_t* hc, uint32_t port, uint8_t speed) {
    int di = -1;
    for (int i = 0; i < MAX_DEVICES; i++) if (!g_devs[i].used) { di = i; break; }
    if (di < 0) return false;
    xhci_dev_t* d = &g_devs[di];
    memset(d, 0, sizeof(*d));
    d->hc = hc;
    d->port = (uint8_t)port;
    d->speed = speed;

    uint32_t slot = 0;
    if (!command(hc, 0, TRB_ENABLE_SLOT << 10, &slot) || !slot || slot > hc->max_slots) {
        serial_write_str("xhci: enable slot failed\n");
        return false;
    }
    d->slot = (uint8_t)slot;
    d->out_ctx = (uint8_t*)alloc_page_zeroed();
    d->in_ctx = (uint8_t*)alloc_page_zeroed();
    d->report = (uint8_t*)alloc_page_zeroed();
    if (!d->out_ctx || !d->in_ctx || !d->report || !ring_init(&d->ep0) || !ring_init(&d->intr))
        return false;
    hc->dcbaa[slot] = (uint64_t)(uintptr_t)d->out_ctx;

    // Address the device: slot context + EP0.
    uint32_t* icc = ctx_at(hc, d->in_ctx, 0);
    icc[1] = 0x3;                                   // add slot + EP0
    uint32_t* sc = ctx_at(hc, d->in_ctx, 1);
    sc[0] = (1u << 27) | ((uint32_t)speed << 20);   // 1 context entry
    sc[1] = (port & 0xFF) << 16;
    uint32_t* ep0 = ctx_at(hc, d->in_ctx, 2);
    ep0[1] = (3u << 1) | (4u << 3) | (ep0_packet_size(speed) << 16);  // CErr 3, control
    uint64_t deq = (uint64_t)(uintptr_t)d->ep0.trbs | 1u;
    ep0[2] = (uint32_t)deq;
    ep0[3] = (uint32_t)(deq >> 32);
    ep0[4] = 8;                                     // average TRB length
    if (!command(hc, (uint64_t)(uintptr_t)d->in_ctx,
                 (TRB_ADDRESS_DEV << 10) | (slot << 24), NULL)) {
        serial_write_str("xhci: address device failed\n");
        return false;
    }

    uint8_t* buf = d->report;                       // reused for descriptors
    if (!control(d, 0x80, 6, 0x0100, 0, 8, buf)) {
        serial_write_str("xhci: device descriptor failed\n");
        return false;
    }
    uint32_t mps = buf[7];
    if (speed == 1 && mps && mps != 8) {            // full speed: real EP0 size
        memset(d->in_ctx, 0, 4096);
        icc = ctx_at(hc, d->in_ctx, 0);
        icc[1] = 0x2;                               // EP0
        ep0 = ctx_at(hc, d->in_ctx, 2);
        ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);
        command(hc, (uint64_t)(uintptr_t)d->in_ctx, (TRB_EVALUATE_CTX << 10) | (slot << 24), NULL);
    }

    // Configuration descriptor (whole thing, up to 512 bytes).
    if (!control(d, 0x80, 6, 0x0200, 0, 9, buf)) return false;
    uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
    if (total > 512) total = 512;
    if (!control(d, 0x80, 6, 0x0200, 0, total, buf)) return false;
    uint8_t config_value = buf[5];

    // First HID boot keyboard or mouse interface and its interrupt IN endpoint.
    int iface = -1, protocol = 0;
    uint8_t ep_addr = 0, interval = 10;
    uint16_t ep_mps = 8;
    for (uint32_t off = 0; off + 2 <= total && buf[off] >= 2; off += buf[off]) {
        uint8_t len = buf[off], type = buf[off + 1];
        if (type == 4 && len >= 9) {                // interface
            if (iface >= 0 && ep_addr) break;
            if (buf[off + 5] == 3 && buf[off + 6] == 1 &&
                (buf[off + 7] == 1 || buf[off + 7] == 2)) {
                iface = buf[off + 2];
                protocol = buf[off + 7];
            } else if (!ep_addr) {
                iface = -1;
            }
        } else if (type == 5 && len >= 7 && iface >= 0 && !ep_addr) {
            if ((buf[off + 2] & 0x80) && (buf[off + 3] & 3) == 3) {
                ep_addr = buf[off + 2];
                ep_mps = (uint16_t)((buf[off + 4] | (buf[off + 5] << 8)) & 0x7FF);
                interval = buf[off + 6];
            }
        }
    }
    if (iface < 0 || !ep_addr) {
        serial_write_str("xhci: device is not a boot keyboard or mouse, ignored\n");
        d->used = false;
        return false;
    }
    d->keyboard = protocol == 1;
    d->report_size = ep_mps > 64 ? 64 : ep_mps;

    if (!control(d, 0x00, 9, config_value, 0, 0, NULL)) return false;    // SET_CONFIGURATION
    control(d, 0x21, 0x0B, 0, (uint16_t)iface, 0, NULL);                  // SET_PROTOCOL boot
    if (d->keyboard) control(d, 0x21, 0x0A, 0, (uint16_t)iface, 0, NULL); // SET_IDLE

    // Configure the interrupt endpoint.
    uint32_t ep_num = ep_addr & 0x0F;
    d->dci = (uint8_t)(ep_num * 2 + 1);
    uint32_t xinterval;
    if (speed >= 3) {
        xinterval = interval ? interval - 1u : 0;
    } else {
        uint32_t frames = interval ? interval : 1;  // ms -> 125 us units, log2
        uint32_t units = frames * 8, l = 0;
        while ((2u << l) <= units) l++;
        xinterval = l < 3 ? 3 : (l > 10 ? 10 : l);
    }
    memset(d->in_ctx, 0, 4096);
    icc = ctx_at(hc, d->in_ctx, 0);
    icc[1] = (1u << d->dci) | 1u;                   // slot + endpoint
    sc = ctx_at(hc, d->in_ctx, 1);
    sc[0] = ((uint32_t)d->dci << 27) | ((uint32_t)speed << 20);
    sc[1] = (port & 0xFF) << 16;
    uint32_t* epc = ctx_at(hc, d->in_ctx, 1 + d->dci);
    epc[0] = xinterval << 16;
    epc[1] = (3u << 1) | (7u << 3) | ((uint32_t)ep_mps << 16);  // interrupt IN
    deq = (uint64_t)(uintptr_t)d->intr.trbs | 1u;
    epc[2] = (uint32_t)deq;
    epc[3] = (uint32_t)(deq >> 32);
    epc[4] = ((uint32_t)ep_mps << 16) | ep_mps;     // max ESIT payload, avg TRB len
    if (!command(hc, (uint64_t)(uintptr_t)d->in_ctx, (TRB_CONFIG_EP << 10) | (slot << 24), NULL)) {
        serial_write_str("xhci: configure endpoint failed\n");
        return false;
    }

    d->used = true;
    queue_report(d);
    serial_write_str(d->keyboard ? "xhci: USB keyboard ready on port "
                                 : "xhci: USB mouse ready on port ");
    serial_write_dec(port);
    serial_write_str("\n");
    return true;
}

static void scan_ports(xhci_t* hc) {
    for (uint32_t port = 1; port <= hc->max_ports; port++) {
        volatile uint8_t* pr = port_reg(hc, port);
        uint32_t sc = r32(pr, 0);
        if (!(sc & 1u)) continue;                    // nothing connected
        // Preserve only read-only and read/write bits; never write 1 to
        // PED or the change bits by accident.
        uint32_t neutral = sc & 0x0E00C3E0u & ~(1u << 1);
        if (!(sc & (1u << 1))) {                     // not enabled: reset it
            w32(pr, 0, neutral | (1u << 4));
            for (uint32_t i = 0; i < 500; i++) {
                udelay(1000);
                if (r32(pr, 0) & (1u << 21)) break;  // reset change
            }
            sc = r32(pr, 0);
            w32(pr, 0, (sc & 0x0E00C3E0u & ~(1u << 1)) | (1u << 21));   // clear PRC
            udelay(20000);
            sc = r32(pr, 0);
        }
        if (!(sc & (1u << 1))) continue;
        uint8_t speed = (uint8_t)((sc >> 10) & 0xF);
        setup_device(hc, port, speed);
    }
}

/* ------------------------------------------------------------ controller */

static void bios_handoff(xhci_t* hc) {
    uint32_t hcc = r32(hc->cap, 0x10);
    uint32_t off = (hcc >> 16) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t v = r32(hc->cap, off);
        if ((v & 0xFF) == 1) {                       // USB legacy support
            w32(hc->cap, off, v | (1u << 24));        // OS owned
            for (uint32_t i = 0; i < 1000; i++) {
                if (!(r32(hc->cap, off) & (1u << 16))) break;  // BIOS released
                udelay(1000);
            }
            // Stop the firmware's SMIs.
            uint32_t ctl = r32(hc->cap, off + 4);
            w32(hc->cap, off + 4, (ctl & 0xFFFF1F0Eu) | 0xE0000000u);
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) return;
        off += next << 2;
    }
}

static bool init_controller(pci_device_t* pci, xhci_t* hc) {
    memset(hc, 0, sizeof(*hc));
    if (pci->bar_type[0] != PCI_BAR_MEM || !pci->bar_address[0]) return false;
    pci_enable_io_busmaster(pci);
    uint64_t len = pci->bar_length[0] ? pci->bar_length[0] : 0x10000;
    if (len > 0x100000) len = 0x100000;
    void* va = mmio_alloc_va(len);
    if (!va || !map_mmio_page((uintptr_t)va, pci->bar_address[0], len)) return false;
    hc->cap = (volatile uint8_t*)va;
    uint8_t caplen = hc->cap[0];
    hc->op = hc->cap + caplen;
    hc->rt = hc->cap + (r32(hc->cap, 0x18) & ~0x1Fu);
    hc->db = (volatile uint32_t*)(hc->cap + (r32(hc->cap, 0x14) & ~0x3u));
    uint32_t hcs1 = r32(hc->cap, 0x04), hcs2 = r32(hc->cap, 0x08), hcc = r32(hc->cap, 0x10);
    hc->max_slots = hcs1 & 0xFF;
    if (hc->max_slots > 32) hc->max_slots = 32;
    hc->max_ports = hcs1 >> 24;
    hc->ctx_size = (hcc & (1u << 2)) ? 64 : 32;

    if (r32(hc->op, 0x04) == 0xFFFFFFFFu) {
        serial_write_str("xhci: registers read all ones (powered off?), skipping\n");
        return false;
    }
    // After power-up (or D3 -> D0) nothing may be written before the
    // controller clears CNR (xHCI 1.2, 4.2).
    for (uint32_t i = 0; i < 1000 && (r32(hc->op, 0x04) & (1u << 11)); i++) udelay(1000);

    bios_handoff(hc);

    // Stop and reset.
    w32(hc->op, 0x00, r32(hc->op, 0x00) & ~1u);
    for (uint32_t i = 0; i < 100 && !(r32(hc->op, 0x04) & 1u); i++) udelay(1000);
    w32(hc->op, 0x00, 1u << 1);
    for (uint32_t i = 0; i < 1000 && (r32(hc->op, 0x00) & (1u << 1)); i++) udelay(1000);
    for (uint32_t i = 0; i < 1000 && (r32(hc->op, 0x04) & (1u << 11)); i++) udelay(1000);
    if (r32(hc->op, 0x04) & (1u << 11)) {
        serial_write_str("xhci: controller not ready after reset (USBSTS=0x");
        serial_write_hex(r32(hc->op, 0x04));
        serial_write_str(", USBCMD=0x");
        serial_write_hex(r32(hc->op, 0x00));
        serial_write_str(")\n");
        return false;
    }

    hc->dcbaa = (uint64_t*)alloc_page_zeroed();
    if (!hc->dcbaa || !ring_init(&hc->cmd)) return false;
    uint32_t scratch = ((hcs2 >> 21) & 0x1F) << 5 | (hcs2 >> 27);
    if (scratch) {
        uint64_t* arr = (uint64_t*)alloc_page_zeroed();
        if (!arr || scratch > 512) return false;
        for (uint32_t i = 0; i < scratch; i++) {
            void* page = alloc_page_zeroed();
            if (!page) return false;
            arr[i] = (uint64_t)(uintptr_t)page;
        }
        hc->dcbaa[0] = (uint64_t)(uintptr_t)arr;
    }

    // Event ring: one segment.
    hc->events = (trb_t*)alloc_page_zeroed();
    uint64_t* erst = (uint64_t*)alloc_page_zeroed();
    if (!hc->events || !erst) return false;
    erst[0] = (uint64_t)(uintptr_t)hc->events;
    erst[1] = RING_TRBS;
    hc->ev_cycle = 1;
    w32(hc->rt, 0x20 + 0x08, 1);                                  // ERSTSZ
    w64(hc->rt, 0x20 + 0x18, (uint64_t)(uintptr_t)hc->events);   // ERDP
    w64(hc->rt, 0x20 + 0x10, (uint64_t)(uintptr_t)erst);         // ERSTBA

    w32(hc->op, 0x38, hc->max_slots);                             // CONFIG
    w64(hc->op, 0x30, (uint64_t)(uintptr_t)hc->dcbaa);            // DCBAAP
    w64(hc->op, 0x18, (uint64_t)(uintptr_t)hc->cmd.trbs | 1u);    // CRCR, RCS
    w32(hc->op, 0x00, 1u);                                        // run, no IRQs
    for (uint32_t i = 0; i < 100 && (r32(hc->op, 0x04) & 1u); i++) udelay(1000);

    // Power the ports (no-op where power is not switchable) and let
    // connections settle.
    for (uint32_t port = 1; port <= hc->max_ports; port++) {
        volatile uint8_t* pr = port_reg(hc, port);
        uint32_t sc = r32(pr, 0);
        if (!(sc & (1u << 9))) w32(pr, 0, (sc & 0x0E00C3E0u & ~(1u << 1)) | (1u << 9));
    }
    udelay(100000);
    hc->ready = true;
    scan_ports(hc);
    return true;
}

uint32_t xhci_init_all(void) {
    extern pci_device_t pci_devices[];
    extern int pci_device_count;
    for (int i = 0; i < pci_device_count && g_hc_count < MAX_CONTROLLERS; i++) {
        pci_device_t* pci = &pci_devices[i];
        if (pci->class_code != 0x0C || pci->subclass != 0x03 || pci->prog_if != 0x30) continue;
        serial_write_str("xhci: controller found\n");
        if (init_controller(pci, &g_hcs[g_hc_count])) g_hc_count++;
    }
    uint32_t devices = 0;
    for (int i = 0; i < MAX_DEVICES; i++) devices += g_devs[i].used;
    g_polling_on = g_hc_count > 0;
    return devices;
}

// Called from the timer interrupt.
void xhci_poll(void) {
    if (!g_polling_on) return;
    for (uint32_t i = 0; i < g_hc_count; i++) {
        if (g_hcs[i].ready) process_events(&g_hcs[i]);
    }
}
