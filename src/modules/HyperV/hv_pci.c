#include "x86_64/hyperv.h"
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/acpi.h"
#include "x86_64/memlayout.h"
#include "x86_64/multiboot2parse.h"
#include "x86_64/spinlock.h"
#include "serial.h"
#include "string.h"

/*
 * Hyper-V virtual PCI (the "PCI pass-through" VMBus device).
 *
 * A device given to a VM with Discrete Device Assignment does not show up
 * on the VM's emulated PCI bus. Each one gets a VMBus channel of class
 * 44c4f61d-4444-4400-9d52-802e27ede19f instead, which describes a small
 * PCI bus:
 *
 *  1. negotiate the protocol version,
 *  2. the host reports the functions on the bus (BUS_RELATIONS),
 *  3. D0 entry tells the host where the guest put an 8 KiB config window:
 *     writing a function's slot number to its first page selects the
 *     function, the second page is then that function's config space,
 *  4. the host reports what each BAR needs (QUERY_RESOURCE_REQUIREMENTS);
 *     the guest picks addresses inside the VM's MMIO gaps (from ACPI),
 *     says it is done (RESOURCES_ASSIGNED), writes the BARs and turns on
 *     memory decoding.
 *
 * Linux does the same in drivers/pci/controller/pci-hyperv.c.
 * The functions are then added to pci_devices[] on a virtual bus, so the
 * normal drivers (GPU, xHCI, ...) find them like any other device.
 */

#define PCI_MSG(n)                    (0x42490000u + (n))
#define PCI_BUS_RELATIONS             PCI_MSG(0x00)
#define PCI_QUERY_BUS_RELATIONS       PCI_MSG(0x01)
#define PCI_QUERY_RESOURCE_REQUIREMENTS PCI_MSG(0x05)
#define PCI_BUS_D0ENTRY               PCI_MSG(0x07)
#define PCI_EJECT                     PCI_MSG(0x0B)
#define PCI_RESOURCES_ASSIGNED        PCI_MSG(0x10)
#define PCI_QUERY_PROTOCOL_VERSION    PCI_MSG(0x13)
#define PCI_RESOURCES_ASSIGNED2       PCI_MSG(0x16)
#define PCI_BUS_RELATIONS2            PCI_MSG(0x19)

#define PCI_VERSION_1_1               0x00010001u
#define PCI_VERSION_1_2               0x00010002u

#define CFG_WINDOW_SIZE               0x2000
#define CFG_PAGE_OFFSET               0x1000

#define HV_PCI_MAX_BUSES              4
#define HV_PCI_MAX_FUNCS              8

static const hv_guid_t g_vpci_class = { {
    0x1d, 0xf6, 0xc4, 0x44, 0x44, 0x44, 0x00, 0x44,
    0x9d, 0x52, 0x80, 0x2e, 0x27, 0xed, 0xe1, 0x9f } };

typedef struct {
    uint16_t vendor_id, device_id;
    uint8_t revision, prog_if, subclass, base_class;
    uint32_t subsystem;
    uint32_t wslot;               // bits 0-4 device, 5-7 function
    uint32_t serial;
    uint32_t probed_bar[6];
} hv_pci_func_t;

typedef struct {
    vmbus_channel_t* ch;
    uint8_t bus;                  // our virtual bus number
    uint32_t version;
    uint64_t cfg_phys;
    volatile uint8_t* cfg;
    bool have_relations;
    uint32_t func_count;
    hv_pci_func_t funcs[HV_PCI_MAX_FUNCS];
} hv_pci_bus_t;

static hv_pci_bus_t g_buses[HV_PCI_MAX_BUSES];
static uint32_t g_bus_count;
static uint64_t g_next_trans = 1;
static spinlock_t g_cfg_lock = SPINLOCK_INIT;

static void log_str_hex(const char* a, uint64_t v, const char* b) {
    serial_write_str(a);
    serial_write_str("0x");
    serial_write_hex(v);
    serial_write_str(b);
}

/* --------------------------------------------------- MMIO ranges (ACPI) */

typedef struct {
    uint64_t base, size;
    uint64_t next;                // bump pointer
} hv_range_t;

static hv_range_t g_ranges[16];
static uint32_t g_range_count;

static void add_range(uint64_t base, uint64_t size) {
    if (size < (1ull << 20)) return;              // only real MMIO windows
    for (uint32_t i = 0; i < g_range_count; i++) {
        if (g_ranges[i].base == base && g_ranges[i].size == size) return;
    }
    if (g_range_count >= sizeof(g_ranges) / sizeof(g_ranges[0])) return;
    g_ranges[g_range_count++] = (hv_range_t){ base, size, base };
    log_str_hex("hv_pci: ACPI MMIO window ", base, "");
    log_str_hex(" size ", size, "\n");
}

static void read_le(const uint8_t* p, void* out, int n) { memcpy(out, p, n); }

// Walks a resource template and records memory address ranges.
static bool parse_resources(const uint8_t* p, const uint8_t* end, bool record) {
    while (p < end) {
        uint8_t tag = *p;
        if (tag & 0x80) {                         // large item
            if (end - p < 3) return false;
            uint16_t len;
            read_le(p + 1, &len, 2);
            const uint8_t* body = p + 3;
            if (body + len > end) return false;
            uint8_t kind = tag & 0x7F;
            if (record && kind == 0x0A && len >= 43 && body[0] == 0) {        // QWord address
                uint64_t min, max, size;
                read_le(body + 11, &min, 8);
                read_le(body + 19, &max, 8);
                read_le(body + 35, &size, 8);
                if (!size && max > min) size = max - min + 1;
                add_range(min, size);
            } else if (record && kind == 0x07 && len >= 23 && body[0] == 0) { // DWord address
                uint32_t min, max, size;
                read_le(body + 7, &min, 4);
                read_le(body + 11, &max, 4);
                read_le(body + 19, &size, 4);
                if (!size && max > min) size = max - min + 1;
                add_range(min, size);
            } else if (record && kind == 0x06 && len >= 9) {                 // 32-bit fixed memory
                uint32_t base, size;
                read_le(body + 1, &base, 4);
                read_le(body + 5, &size, 4);
                add_range(base, size);
            }
            p = body + len;
        } else {                                  // small item
            uint8_t len = tag & 7;
            if (((tag >> 3) & 0xF) == 0xF) return p + 2 <= end;  // end tag
            p += 1 + len;
        }
    }
    return false;
}

// Every Name(XXXX, ResourceTemplate () {...}) in an AML table.
static void scan_aml(const uint8_t* t, uint32_t length) {
    for (uint32_t i = 36; i + 8 < length; i++) {
        if (t[i] != 0x08 || t[i + 5] != 0x11) continue;          // NameOp .... BufferOp
        bool name_ok = true;
        for (int k = 1; k <= 4; k++) {
            uint8_t c = t[i + k];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) name_ok = false;
        }
        if (!name_ok) continue;
        const uint8_t* p = t + i + 6;
        uint32_t lead = p[0], count = lead >> 6, pkglen;
        if (count == 0) pkglen = lead & 0x3F;
        else {
            pkglen = lead & 0x0F;
            for (uint32_t k = 0; k < count; k++) pkglen |= (uint32_t)p[1 + k] << (4 + 8 * k);
        }
        const uint8_t* end = p + pkglen;
        if (end > t + length || pkglen < 4) continue;
        const uint8_t* q = p + 1 + count;                         // buffer size term
        if (*q == 0x0A) q += 2;
        else if (*q == 0x0B) q += 3;
        else if (*q == 0x0C) q += 5;
        else if (*q == 0x00 || *q == 0x01) q += 1;
        else continue;
        if (q >= end) continue;
        if (parse_resources(q, end, false)) parse_resources(q, end, true);
    }
}

static void find_mmio_ranges(void) {
    const uint8_t* dsdt = (const uint8_t*)acpi_find_table(kernel_multiboot_info, "DSDT", 0);
    if (dsdt) scan_aml(dsdt, *(const uint32_t*)(dsdt + 4));
    for (uint32_t i = 0; i < 16; i++) {
        const uint8_t* ssdt = (const uint8_t*)acpi_find_table(kernel_multiboot_info, "SSDT", i);
        if (!ssdt) break;
        scan_aml(ssdt, *(const uint32_t*)(ssdt + 4));
    }
}

static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
    return a < b + blen && b < a + alen;
}

// Something already lives there: RAM or firmware areas from the memory
// map, BARs of other devices, the APIC / HPET / flash area below 4 GiB.
static bool address_busy(uint64_t base, uint64_t size) {
    if (overlaps(base, size, 0xFEC00000ull, 0x01400000ull)) return true;
    multiboot2_info_t* mb = kernel_multiboot_info;
    if (mb) {
        uint8_t* p = mb->tags;
        uint8_t* end = (uint8_t*)mb + mb->total_size;
        while (p < end) {
            multiboot2_tag_t* tag = (multiboot2_tag_t*)p;
            if (tag->type == 0) break;
            if (tag->type == 6) {
                multiboot2_tag_mmap_t* m = (multiboot2_tag_mmap_t*)tag;
                for (uint8_t* e = p + sizeof(*m); e + m->entry_size <= p + tag->size;
                     e += m->entry_size) {
                    multiboot2_mmap_entry_t* me = (multiboot2_mmap_entry_t*)e;
                    if (overlaps(base, size, me->base_addr, me->length)) return true;
                }
            }
            p += (tag->size + 7) & ~7u;
        }
    }
    for (int i = 0; i < pci_device_count; i++) {
        for (int b = 0; b < PCI_NUM_BARS; b++) {
            if (pci_devices[i].bar_type[b] != PCI_BAR_MEM) continue;
            if (overlaps(base, size, pci_devices[i].bar_address[b], pci_devices[i].bar_length[b]))
                return true;
        }
    }
    for (uint32_t i = 0; i < g_bus_count; i++) {
        if (g_buses[i].cfg_phys && overlaps(base, size, g_buses[i].cfg_phys, CFG_WINDOW_SIZE))
            return true;
    }
    return false;
}

// Naturally aligned block inside one of the ACPI windows. `high`: only
// above 4 GiB (64-bit BARs), else only below.
static uint64_t mmio_alloc(uint64_t size, uint64_t align, bool high) {
    if (align < 4096) align = 4096;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < g_range_count; i++) {
            hv_range_t* r = &g_ranges[i];
            bool is_high = r->base >= (1ull << 32);
            if (pass == 0 && is_high != high) continue;
            if (pass == 1 && (is_high || !high)) continue;   // 64-bit BARs may go low
            uint64_t a = (r->next + align - 1) & ~(align - 1);
            for (; a + size <= r->base + r->size; a += align) {
                if (!is_high && a + size > (1ull << 32)) break;
                if (address_busy(a, size)) continue;
                r->next = a + size;
                return a;
            }
        }
    }
    return 0;
}

/* --------------------------------------------------------- channel I/O */

static void handle_relations(hv_pci_bus_t* bus, const uint8_t* m, uint32_t len) {
    uint32_t type, count;
    memcpy(&type, m, 4);
    memcpy(&count, m + 4, 4);
    uint32_t stride = type == PCI_BUS_RELATIONS2 ? 24 : 20;
    bus->func_count = 0;
    for (uint32_t i = 0; i < count && i < HV_PCI_MAX_FUNCS; i++) {
        const uint8_t* d = m + 8 + i * stride;
        if (d + 20 > m + len) break;
        hv_pci_func_t* f = &bus->funcs[bus->func_count++];
        memset(f, 0, sizeof(*f));
        memcpy(&f->vendor_id, d, 2);
        memcpy(&f->device_id, d + 2, 2);
        f->revision = d[4];
        f->prog_if = d[5];
        f->subclass = d[6];
        f->base_class = d[7];
        memcpy(&f->subsystem, d + 8, 4);
        memcpy(&f->wslot, d + 12, 4);
        memcpy(&f->serial, d + 16, 4);
    }
    bus->have_relations = true;
}

// Handles host-initiated packets; returns true for a completion that
// matches `trans` (copied into out).
static bool handle_packet(hv_pci_bus_t* bus, const uint8_t* buf, const vmbus_packet_t* pkt,
                          uint64_t trans, uint8_t* out, uint32_t cap) {
    if (pkt->type == VMBUS_PKT_COMPLETION) {
        if (pkt->trans_id != trans) return false;
        memcpy(out, buf, pkt->len < cap ? pkt->len : cap);
        return true;
    }
    if (pkt->len < 4) return false;
    uint32_t type;
    memcpy(&type, buf, 4);
    if (type == PCI_BUS_RELATIONS || type == PCI_BUS_RELATIONS2) {
        handle_relations(bus, buf, pkt->len);
    } else if (type == PCI_EJECT) {
        serial_write_str("hv_pci: host asks to eject a device (ignored)\n");
    } else {
        log_str_hex("hv_pci: unhandled packet type ", type, "\n");
    }
    return false;
}

// Drains the receive ring until `done` or the timeout.
static bool pump(hv_pci_bus_t* bus, uint64_t trans, uint8_t* out, uint32_t cap,
                 bool want_relations, uint32_t timeout_ms) {
    uint64_t deadline = hv_time_100ns() + (uint64_t)timeout_ms * 10000;
    uint8_t buf[512];
    for (uint64_t spins = 0;; spins++) {
        vmbus_packet_t pkt;
        while (vmbus_recv(bus->ch, buf, sizeof(buf), &pkt)) {
            if (handle_packet(bus, buf, &pkt, trans, out, cap)) return true;
            if (want_relations && bus->have_relations) return true;
        }
        if (hv_time_100ns() >= deadline || spins > 2000000000ull) return false;
        __asm__ volatile("pause");
    }
}

// Sends a message that wants a completion; returns its NTSTATUS (or -1 on
// timeout) and copies the completion payload into out.
static int32_t transact(hv_pci_bus_t* bus, const void* msg, uint32_t len, uint8_t* out,
                        uint32_t cap) {
    uint64_t trans = g_next_trans++;
    uint8_t local[64];
    if (!out) { out = local; cap = sizeof(local); }
    memset(out, 0, cap);
    if (!vmbus_send(bus->ch, msg, len, trans, true)) return -1;
    if (!pump(bus, trans, out, cap, false, 5000)) return -1;
    int32_t status;
    memcpy(&status, out, 4);
    return status;
}

/* ------------------------------------------------------- config window */

static hv_pci_bus_t* bus_for(uint8_t bus) {
    uint32_t i = (uint32_t)(bus - PCI_VBUS_FIRST);
    return i < g_bus_count ? &g_buses[i] : NULL;
}

static hv_pci_func_t* func_for(hv_pci_bus_t* b, uint8_t device, uint8_t function) {
    if (!b) return NULL;
    for (uint32_t i = 0; i < b->func_count; i++) {
        uint32_t s = b->funcs[i].wslot;
        if ((s & 0x1F) == device && ((s >> 5) & 7) == function) return &b->funcs[i];
    }
    return NULL;
}

static uint32_t cfg_read(uint8_t busno, uint8_t device, uint8_t function, uint16_t offset) {
    hv_pci_bus_t* b = bus_for(busno);
    hv_pci_func_t* f = func_for(b, device, function);
    if (!f || !b->cfg || offset >= 0x1000) return 0xFFFFFFFFu;
    // IDs, class and subsystem come from the bus relations (as Linux does);
    // the expansion ROM and legacy interrupt line are not passed through.
    switch (offset) {
        case 0x00: return f->vendor_id | ((uint32_t)f->device_id << 16);
        case 0x08: return f->revision | ((uint32_t)f->prog_if << 8) |
                          ((uint32_t)f->subclass << 16) | ((uint32_t)f->base_class << 24);
        case 0x2C: return f->subsystem;
        case 0x30: return 0;
        default: break;
    }
    uint64_t flags = spinlock_acquire(&g_cfg_lock);
    *(volatile uint32_t*)b->cfg = f->wslot;
    __asm__ volatile("mfence" ::: "memory");
    uint32_t v = *(volatile uint32_t*)(b->cfg + CFG_PAGE_OFFSET + offset);
    spinlock_release(&g_cfg_lock, flags);
    if (offset == 0x3C) v &= 0xFFFF0000u;   // no INTx line / pin
    return v;
}

static void cfg_write(uint8_t busno, uint8_t device, uint8_t function, uint16_t offset,
                      uint32_t value) {
    hv_pci_bus_t* b = bus_for(busno);
    hv_pci_func_t* f = func_for(b, device, function);
    if (!f || !b->cfg || offset >= 0x1000) return;
    if (offset < 0x04 || offset == 0x08 || offset == 0x2C || offset == 0x30) return;  // read-only
    uint64_t flags = spinlock_acquire(&g_cfg_lock);
    *(volatile uint32_t*)b->cfg = f->wslot;
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t*)(b->cfg + CFG_PAGE_OFFSET + offset) = value;
    __asm__ volatile("mfence" ::: "memory");
    spinlock_release(&g_cfg_lock, flags);
}

/* -------------------------------------------------------------- bring-up */

static bool negotiate(hv_pci_bus_t* b) {
    static const uint32_t versions[] = { 0x00010004, 0x00010003, 0x00010002, 0x00010001 };
    for (uint32_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        uint32_t msg[2] = { PCI_QUERY_PROTOCOL_VERSION, versions[i] };
        int32_t st = transact(b, msg, sizeof(msg), NULL, 0);
        if (st >= 0) {
            b->version = versions[i];
            return true;
        }
        if (st == -1) return false;                  // no answer at all
    }
    return false;
}

static uint64_t bar_size(const uint32_t* probed, int i, bool* is64) {
    uint32_t lo = probed[i];
    *is64 = (lo & 0x7) == 0x4 && i < 5;
    if (lo & 1) return 0;                            // I/O BAR: not supported
    uint64_t mask = lo & ~0xFull;
    if (*is64) mask |= (uint64_t)probed[i + 1] << 32;
    else if (mask) mask |= 0xFFFFFFFF00000000ull;
    return mask ? (~mask + 1) : 0;
}

static uint32_t bring_up_bus(hv_pci_bus_t* b) {
    if (!vmbus_channel_open(b->ch, 4, 4)) {
        serial_write_str("hv_pci: cannot open channel\n");
        return 0;
    }
    if (!negotiate(b)) {
        serial_write_str("hv_pci: protocol negotiation failed\n");
        return 0;
    }
    log_str_hex("hv_pci: protocol ", b->version, "\n");

    // Functions on the bus. The host may have sent them already.
    if (!b->have_relations) {
        uint32_t q = PCI_QUERY_BUS_RELATIONS;
        vmbus_send(b->ch, &q, sizeof(q), 0, false);
        if (!pump(b, 0, NULL, 0, true, 5000)) {
            serial_write_str("hv_pci: no bus relations\n");
            return 0;
        }
    }

    // Config window, then D0.
    b->cfg_phys = mmio_alloc(CFG_WINDOW_SIZE, 0x1000, false);
    if (!b->cfg_phys) b->cfg_phys = mmio_alloc(CFG_WINDOW_SIZE, 0x1000, true);
    if (!b->cfg_phys) {
        serial_write_str("hv_pci: no MMIO space for the config window\n");
        return 0;
    }
    void* va = mmio_alloc_va(CFG_WINDOW_SIZE);
    if (!va || !map_mmio_page((uintptr_t)va, b->cfg_phys, CFG_WINDOW_SIZE)) return 0;
    b->cfg = (volatile uint8_t*)va;
    struct __attribute__((packed)) { uint32_t type, reserved; uint64_t mmio_base; } d0 = {
        PCI_BUS_D0ENTRY, 0, b->cfg_phys };
    int32_t st = transact(b, &d0, sizeof(d0), NULL, 0);
    if (st < 0) {
        log_str_hex("hv_pci: D0 entry failed, status ", (uint32_t)st, "\n");
        return 0;
    }
    log_str_hex("hv_pci: config window at ", b->cfg_phys, "\n");

    uint32_t added = 0;
    for (uint32_t i = 0; i < b->func_count; i++) {
        hv_pci_func_t* f = &b->funcs[i];
        uint32_t q[2] = { PCI_QUERY_RESOURCE_REQUIREMENTS, f->wslot };
        uint8_t resp[64];
        st = transact(b, q, sizeof(q), resp, sizeof(resp));
        if (st < 0) {
            log_str_hex("hv_pci: resource query failed, status ", (uint32_t)st, "\n");
            continue;
        }
        memcpy(f->probed_bar, resp + 4, sizeof(f->probed_bar));

        pci_device_t dev;
        memset(&dev, 0, sizeof(dev));
        dev.bus = b->bus;
        dev.device = f->wslot & 0x1F;
        dev.function = (f->wslot >> 5) & 7;
        dev.vendor_id = f->vendor_id;
        dev.device_id = f->device_id;
        dev.class_code = f->base_class;
        dev.subclass = f->subclass;
        dev.prog_if = f->prog_if;

        for (int bar = 0; bar < 6; bar++) {
            bool is64 = false;
            uint64_t size = bar_size(f->probed_bar, bar, &is64);
            if (size) {
                uint64_t addr = mmio_alloc(size, size, is64);
                if (!addr) {
                    log_str_hex("hv_pci: no MMIO space for a BAR of size ", size, "\n");
                } else {
                    dev.bar_address[bar] = addr;
                    dev.bar_length[bar] = size;
                    dev.bar[bar] = (uint32_t)addr;
                    dev.bar_type[bar] = PCI_BAR_MEM;
                    dev.bar_is_64[bar] = is64;
                    dev.bar_prefetchable[bar] = (f->probed_bar[bar] & 0x8) != 0;
                }
            }
            if (is64) bar++;
        }

        // Tell the host the guest has placed the device (the message's
        // resource lists are not used), then program the BARs.
        uint8_t ra[202];
        memset(ra, 0, sizeof(ra));
        uint32_t type = b->version >= PCI_VERSION_1_2 ? PCI_RESOURCES_ASSIGNED2
                                                      : PCI_RESOURCES_ASSIGNED;
        memcpy(ra, &type, 4);
        memcpy(ra + 4, &f->wslot, 4);
        st = transact(b, ra, b->version >= PCI_VERSION_1_2 ? 202 : 148, NULL, 0);
        if (st < 0) {
            log_str_hex("hv_pci: resources-assigned refused, status ", (uint32_t)st, "\n");
            continue;
        }

        uint32_t cmd = cfg_read(b->bus, dev.device, dev.function, 0x04);
        cfg_write(b->bus, dev.device, dev.function, 0x04, cmd & ~0x7u);
        for (int bar = 0; bar < 6; bar++) {
            if (dev.bar_type[bar] != PCI_BAR_MEM) continue;
            uint64_t a = dev.bar_address[bar];
            if (dev.bar_is_64[bar]) {
                cfg_write(b->bus, dev.device, dev.function, 0x14 + bar * 4, (uint32_t)(a >> 32));
            }
            cfg_write(b->bus, dev.device, dev.function, 0x10 + bar * 4,
                      (uint32_t)a | (f->probed_bar[bar] & 0xF));
        }
        cmd = cfg_read(b->bus, dev.device, dev.function, 0x04);
        cfg_write(b->bus, dev.device, dev.function, 0x04, (cmd & 0xFFFF) | 0x6);   // memory + bus master
        dev.header_type = (uint8_t)(cfg_read(b->bus, dev.device, dev.function, 0x0C) >> 16);

        serial_write_str("hv_pci: device ");
        serial_write_hex(dev.vendor_id);
        serial_write_str(":");
        serial_write_hex(dev.device_id);
        serial_write_str(" class ");
        serial_write_hex(dev.class_code);
        serial_write_str(" at vbus ");
        serial_write_hex(dev.bus);
        serial_write_str("\n");
        for (int bar = 0; bar < 6; bar++) {
            if (dev.bar_type[bar] != PCI_BAR_MEM) continue;
            serial_write_str("hv_pci:   BAR");
            serial_write_dec((uint64_t)bar);
            log_str_hex(" ", dev.bar_address[bar], "");
            log_str_hex(" size ", dev.bar_length[bar], "");
            log_str_hex(" reads back ", cfg_read(b->bus, dev.device, dev.function, 0x10 + bar * 4), "\n");
        }
        if (pci_add_device(&dev)) added++;
    }
    return added;
}

uint32_t hv_pci_init(void) {
    if (!vmbus_find(&g_vpci_class, NULL)) return 0;
    find_mmio_ranges();
    if (!g_range_count) serial_write_str("hv_pci: ACPI lists no MMIO windows\n");
    pci_set_vbus_accessors(cfg_read, cfg_write);

    uint32_t added = 0;
    for (vmbus_channel_t* ch = vmbus_find(&g_vpci_class, NULL);
         ch && g_bus_count < HV_PCI_MAX_BUSES; ch = vmbus_find(&g_vpci_class, ch)) {
        hv_pci_bus_t* b = &g_buses[g_bus_count];
        memset(b, 0, sizeof(*b));
        b->ch = ch;
        b->bus = (uint8_t)(PCI_VBUS_FIRST + g_bus_count);
        g_bus_count++;
        added += bring_up_bus(b);
    }
    return added;
}
