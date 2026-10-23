#include <stdint.h>
#include <stdbool.h>
#include "x86_64/rtl8168.h"
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/pmm.h"
#include "serial.h"
#include "string.h"
#include "net/ethernet.h"
#include "net/net.h"

/*
 * Realtek RTL8111/8168/8411 PCI Express gigabit Ethernet (10ec:8168, and
 * 10ec:8161), the NIC on most AMD and Intel desktop boards.
 *
 * Polled like the RTL8139 driver: no interrupts. Frames move through two
 * rings of 16-byte descriptors in RAM; the card owns a descriptor while
 * its OWN bit is set. The start-up order follows Linux's r8169
 * (rtl_hw_start): reset, unlock the config registers, ring addresses,
 * lock, enable Tx/Rx, then the Rx/Tx configuration. Chip-specific PHY
 * tuning and PHY firmware are left out: the link the board firmware
 * brought up is used as is.
 */

#define REG_MAC0         0x00
#define REG_MAR0         0x08    // multicast filter, 8 bytes
#define REG_TX_DESC_LO   0x20
#define REG_TX_DESC_HI   0x24
#define REG_CHIP_CMD     0x37
#define REG_TX_POLL      0x38
#define REG_INTR_MASK    0x3C
#define REG_INTR_STATUS  0x3E
#define REG_TX_CONFIG    0x40
#define REG_RX_CONFIG    0x44
#define REG_CFG9346      0x50
#define REG_PHY_STATUS   0x6C
#define REG_RX_MAX_SIZE  0xDA
#define REG_CPLUS_CMD    0xE0
#define REG_RX_DESC_LO   0xE4
#define REG_RX_DESC_HI   0xE8
#define REG_MAX_TX_SIZE  0xEC

#define CMD_RESET        0x10
#define CMD_RX_ENABLE    0x08
#define CMD_TX_ENABLE    0x04
#define TX_POLL_NORMAL   0x40
#define CFG9346_UNLOCK   0xC0
#define CFG9346_LOCK     0x00
#define PHY_LINK_UP      0x02

// RxConfig: accept everything (filtering happens in software), unlimited
// DMA bursts, the 8168's multi-descriptor fetch and no early Rx.
#define RX_ACCEPT_ALL    0x0F            // all/physical/multicast/broadcast
#define RX_CONFIG        ((1u << 15) | (1u << 14) | (1u << 11) | (7u << 8) | RX_ACCEPT_ALL)
// TxConfig: standard inter-frame gap, unlimited DMA bursts, automatic
// Tx FIFO (RTL8168e-vl and newer; ignored by older chips).
#define TX_CONFIG        ((3u << 24) | (7u << 8) | (1u << 7))

#define DESC_OWN         (1u << 31)
#define DESC_END         (1u << 30)      // last descriptor of the ring
#define DESC_FIRST       (1u << 29)
#define DESC_LAST        (1u << 28)
#define RX_DESC_ERROR    (1u << 21)      // RES: CRC, runt, ... (with bits 15-20)
#define DESC_LEN_MASK    0x3FFFu

#define RX_COUNT         64
#define TX_COUNT         16
#define BUF_SIZE         2048            // per descriptor, one frame each

typedef struct {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint64_t addr;
} __attribute__((packed)) rtl_desc_t;

static volatile uint8_t* g_regs;
static rtl_desc_t* g_rx;                 // RX_COUNT descriptors, 256-byte aligned
static rtl_desc_t* g_tx;
static uint8_t* g_rx_buf[RX_COUNT];
static uint8_t* g_tx_buf[TX_COUNT];
static uint32_t g_rx_next;
static uint32_t g_tx_next;
static uint8_t g_mac[6];
static bool g_present;
static bool g_link_reported;

static inline uint8_t rd8(uint32_t r) { return g_regs[r]; }
static inline uint16_t rd16(uint32_t r) { return *(volatile uint16_t*)(g_regs + r); }
static inline uint32_t rd32(uint32_t r) { return *(volatile uint32_t*)(g_regs + r); }
static inline void wr8(uint32_t r, uint8_t v) { g_regs[r] = v; }
static inline void wr16(uint32_t r, uint16_t v) { *(volatile uint16_t*)(g_regs + r) = v; }
static inline void wr32(uint32_t r, uint32_t v) { *(volatile uint32_t*)(g_regs + r) = v; }

static pci_device_t* find_rtl8168(void) {
    extern pci_device_t pci_devices[];
    extern int pci_device_count;
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* d = &pci_devices[i];
        if (d->vendor_id == 0x10EC && (d->device_id == 0x8168 || d->device_id == 0x8161))
            return d;
    }
    return NULL;
}

// The register window: BAR2 (64-bit memory) on PCIe parts, BAR1 on some.
static bool map_registers(pci_device_t* dev) {
    static const int bars[] = { 2, 1, 4 };
    for (uint32_t i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
        int b = bars[i];
        if (dev->bar_type[b] != PCI_BAR_MEM || !dev->bar_address[b]) continue;
        uint64_t phys = dev->bar_address[b] & ~0xFFFull;
        void* va = mmio_alloc_va(0x1000);
        if (!va || !map_mmio_page((uintptr_t)va, (uintptr_t)phys, 0x1000)) return false;
        g_regs = (volatile uint8_t*)va + (dev->bar_address[b] & 0xFFF);
        return true;
    }
    return false;
}

static bool wait_reset_done(void) {
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (!(rd8(REG_CHIP_CMD) & CMD_RESET)) return true;
        asm volatile("pause");
    }
    return false;
}

static void rx_give(uint32_t i) {
    g_rx[i].opts2 = 0;
    g_rx[i].addr = (uint64_t)(uintptr_t)g_rx_buf[i];
    // The address is written before ownership moves to the card.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_rx[i].opts1 = DESC_OWN | (i == RX_COUNT - 1 ? DESC_END : 0) | BUF_SIZE;
}

static bool alloc_rings(void) {
    // One page holds a whole ring (64 x 16 bytes); pages are 256-byte
    // aligned as the card requires. Low RAM is identity-mapped, so these
    // pointers are also the physical addresses the card uses.
    g_rx = (rtl_desc_t*)alloc_page_zeroed();
    g_tx = (rtl_desc_t*)alloc_page_zeroed();
    if (!g_rx || !g_tx) return false;
    for (uint32_t i = 0; i < RX_COUNT; i += 2) {
        uint8_t* page = (uint8_t*)alloc_page_zeroed();
        if (!page) return false;
        g_rx_buf[i] = page;
        g_rx_buf[i + 1] = page + BUF_SIZE;
    }
    for (uint32_t i = 0; i < TX_COUNT; i += 2) {
        uint8_t* page = (uint8_t*)alloc_page_zeroed();
        if (!page) return false;
        g_tx_buf[i] = page;
        g_tx_buf[i + 1] = page + BUF_SIZE;
    }
    for (uint32_t i = 0; i < RX_COUNT; i++) rx_give(i);
    for (uint32_t i = 0; i < TX_COUNT; i++) {
        g_tx[i].opts1 = (i == TX_COUNT - 1) ? DESC_END : 0;
        g_tx[i].addr = (uint64_t)(uintptr_t)g_tx_buf[i];
    }
    return true;
}

static void report_link(void) {
    bool up = rd8(REG_PHY_STATUS) & PHY_LINK_UP;
    serial_write_str(up ? "RTL8168: link up\n" : "RTL8168: no link (cable?)\n");
    g_link_reported = up;
}

static bool rtl8168_init(void) {
    pci_device_t* dev = find_rtl8168();
    if (!dev) return false;
    pci_enable_io_busmaster(dev);
    if (!map_registers(dev)) {
        serial_write_str("RTL8168: no usable register BAR\n");
        return false;
    }

    // Stop everything and reset the MAC.
    wr16(REG_INTR_MASK, 0);
    wr8(REG_CHIP_CMD, CMD_RESET);
    if (!wait_reset_done()) {
        serial_write_str("RTL8168: reset timeout\n");
        return false;
    }
    wr16(REG_INTR_STATUS, 0xFFFF);

    for (int i = 0; i < 6; i++) g_mac[i] = rd8(REG_MAC0 + i);
    if (!alloc_rings()) {
        serial_write_str("RTL8168: out of memory for the rings\n");
        return false;
    }

    uint32_t xid = (rd32(REG_TX_CONFIG) >> 20) & 0xFCF;   // chip version, for the log

    wr8(REG_CFG9346, CFG9346_UNLOCK);
    wr16(REG_CPLUS_CMD, rd16(REG_CPLUS_CMD) & ~((1u << 5) | (1u << 6)));  // no checksum offload, no VLAN strip
    wr8(REG_MAX_TX_SIZE, 0x3F);                            // 8064 bytes, in 128-byte units
    wr16(REG_RX_MAX_SIZE, BUF_SIZE);
    wr32(REG_TX_DESC_HI, (uint32_t)((uint64_t)(uintptr_t)g_tx >> 32));
    wr32(REG_TX_DESC_LO, (uint32_t)(uintptr_t)g_tx);
    wr32(REG_RX_DESC_HI, (uint32_t)((uint64_t)(uintptr_t)g_rx >> 32));
    wr32(REG_RX_DESC_LO, (uint32_t)(uintptr_t)g_rx);
    wr8(REG_CFG9346, CFG9346_LOCK);
    (void)rd16(REG_CPLUS_CMD);                             // flush posted writes

    wr8(REG_CHIP_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);
    wr32(REG_RX_CONFIG, RX_CONFIG);
    wr32(REG_TX_CONFIG, TX_CONFIG);
    wr32(REG_MAR0, 0xFFFFFFFF);                            // all multicast groups
    wr32(REG_MAR0 + 4, 0xFFFFFFFF);
    wr16(REG_INTR_MASK, 0);                                // polled
    wr16(REG_INTR_STATUS, 0xFFFF);

    g_rx_next = 0;
    g_tx_next = 0;
    g_present = true;

    serial_write_str("RTL8168: initialized, XID 0x");
    serial_write_hex(xid);
    serial_write_str(", MAC=");
    for (int i = 0; i < 6; i++) {
        serial_write_hex(g_mac[i]);
        if (i < 5) serial_write_str(":");
    }
    serial_write_str("\n");
    report_link();
    return true;
}

static void rtl8168_get_mac(uint8_t out[6]) {
    memcpy(out, g_mac, 6);
}

static void rtl8168_send(const void* frame, uint16_t len) {
    if (!g_present || !frame) return;
    if (len > ETH_FRAME_MAX) len = ETH_FRAME_MAX;

    uint32_t i = g_tx_next;
    // Wait for the card to finish with this descriptor's previous frame.
    for (uint32_t spin = 0; (g_tx[i].opts1 & DESC_OWN) && spin < 1000000u; spin++) {
        asm volatile("pause");
    }
    if (g_tx[i].opts1 & DESC_OWN) {
        serial_write_str("RTL8168: transmit ring stuck, frame dropped\n");
        return;
    }
    g_tx_next = (i + 1) % TX_COUNT;

    memcpy(g_tx_buf[i], frame, len);
    if (len < 60) {                                         // minimum Ethernet frame
        memset(g_tx_buf[i] + len, 0, 60 - len);
        len = 60;
    }
    g_tx[i].opts2 = 0;
    g_tx[i].addr = (uint64_t)(uintptr_t)g_tx_buf[i];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_tx[i].opts1 = DESC_OWN | DESC_FIRST | DESC_LAST |
                    (i == TX_COUNT - 1 ? DESC_END : 0) | len;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr8(REG_TX_POLL, TX_POLL_NORMAL);
}

static void rtl8168_poll(void) {
    if (!g_present) return;

    // Acknowledge status (Rx OK, ring empty, FIFO overflow, ...): some
    // chips stop receiving until "Rx descriptor unavailable" is cleared.
    uint16_t status = rd16(REG_INTR_STATUS);
    if (status) wr16(REG_INTR_STATUS, status);

    for (uint32_t budget = 0; budget < RX_COUNT; budget++) {
        uint32_t i = g_rx_next;
        uint32_t opts1 = g_rx[i].opts1;
        if (opts1 & DESC_OWN) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        uint32_t len = opts1 & DESC_LEN_MASK;                // includes the 4-byte CRC
        bool whole = (opts1 & (DESC_FIRST | DESC_LAST)) == (DESC_FIRST | DESC_LAST);
        if (whole && !(opts1 & RX_DESC_ERROR) && len > 4 + 14 && len - 4 <= ETH_FRAME_MAX) {
            eth_handle_frame(g_rx_buf[i], (uint16_t)(len - 4));
        }
        rx_give(i);
        g_rx_next = (i + 1) % RX_COUNT;
    }

    if (!g_link_reported && (rd8(REG_PHY_STATUS) & PHY_LINK_UP)) {
        serial_write_str("RTL8168: link up\n");
        g_link_reported = true;
    }
}

static const network_hw_driver_t rtl8168_driver = {
    .name    = "RTL8168",
    .init    = rtl8168_init,
    .send    = rtl8168_send,
    .poll    = rtl8168_poll,
    .get_mac = rtl8168_get_mac,
};

const network_hw_driver_t* rtl8168_get_driver(void) {
    return &rtl8168_driver;
}
