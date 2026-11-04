#include "x86_64/syscall.h"
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
#define REG_PHYAR        0x60    // MII access, chips before the 8168G
#define REG_PHY_STATUS   0x6C
#define REG_PMCH         0x6F    // PLL power
#define REG_GPHY_OCP     0xB8    // PHY access, 8168G and newer
#define REG_MAC_OCP      0xB0
#define REG_MCU          0xD3
#define REG_MISC         0xF0
#define REG_ERIDR        0x70    // extended registers, data
#define REG_ERIAR        0x74    // extended registers, address/command
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
#define OCP_FLAG         (1u << 31)
#define MCU_NOW_IS_OOB   0x80    // MAC still owned by the board firmware
#define MCU_RXTX_EMPTY   0x30
#define MCU_LL_READY     0x02
#define MISC_RXDV_GATE   (1u << 19)  // blocks the receive path when set
#define TXCFG_EMPTY      (1u << 11)
#define BMCR_ANRESTART   (1u << 9)
#define BMCR_PDOWN       (1u << 11)
#define BMCR_ANENABLE    (1u << 12)

// RxConfig: accept everything (filtering happens in software) and
// unlimited DMA bursts. Newer families add multi-descriptor fetch and
// "no early Rx"; see rx_config(). Values follow Linux's rtl_init_rxcfg.
#define RX_ACCEPT_ALL    0x0F            // all/physical/multicast/broadcast
#define RX_128_INT       (1u << 15)
#define RX_MULTI_EN      (1u << 14)
#define RX_EARLY_OFF     (1u << 11)
#define RX_DMA_BURST     (7u << 8)
// TxConfig: standard inter-frame gap, unlimited DMA bursts; automatic
// Tx FIFO only on the RTL8168E-VL and newer.
#define TX_CONFIG        ((3u << 24) | (7u << 8))
#define TX_AUTO_FIFO     (1u << 7)

// Chip families, from the TxConfig XID (Linux rtl_chip_infos).
enum { FAM_OLD, FAM_D, FAM_EVL, FAM_G };

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
static uint32_t g_xid;
static int g_family;
static uint16_t g_bmcr_at_start;
static bool g_was_oob;
static uint32_t g_rx_frames, g_rx_errors, g_tx_frames;
static uint16_t g_status_seen;           // every IntrStatus bit seen since start
static uint32_t g_rx_bcast, g_rx_ucast;  // good frames by destination
static uint32_t g_rx_resyncs;            // times our ring position was repaired

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

// Each MMIO read takes roughly a microsecond on PCIe; good enough as a
// delay base before the timer is trusted.
static void delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) (void)rd8(REG_CHIP_CMD);
}

static int family_of(uint32_t xid) {
    switch (xid & 0x7CF) {
        case 0x4C0: case 0x4C1: case 0x509: case 0x5C8:      // 8168G
        case 0x541: case 0x6C0:                              // 8168H / 8168M
        case 0x502: case 0x503:                              // 8168EP
            return FAM_G;
        case 0x480: case 0x481: case 0x488: case 0x489:      // 8168F, 8411
        case 0x2C8: case 0x2C9:                              // 8168E-VL
            return FAM_EVL;
        default: break;
    }
    switch (xid & 0x7C8) {
        case 0x2C0: case 0x280: case 0x288: return FAM_D;    // 8168D/DP/E
        default: break;
    }
    // Unknown, newer IDs share the 8168G programming model.
    if ((xid & 0x7CF) > 0x4C0 && (xid & 0x7CF) != 0x7CF) return FAM_G;
    return FAM_OLD;
}

static uint32_t rx_config(void) {
    switch (g_family) {
        case FAM_G:   return RX_128_INT | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF | RX_ACCEPT_ALL;
        case FAM_EVL:
        case FAM_D:   return RX_128_INT | RX_MULTI_EN | RX_DMA_BURST | RX_ACCEPT_ALL;
        default:      return RX_128_INT | RX_DMA_BURST | RX_ACCEPT_ALL;
    }
}

static bool wait_set8(uint32_t reg, uint8_t mask, uint32_t us) {
    for (uint32_t i = 0; i < us; i += 10) {
        if ((rd8(reg) & mask) == mask) return true;
        delay_us(10);
    }
    return false;
}

static void mac_ocp_write(uint32_t reg, uint16_t v) {
    wr32(REG_MAC_OCP, OCP_FLAG | ((reg >> 1) << 16) | v);
}

static uint16_t mac_ocp_read(uint32_t reg) {
    wr32(REG_MAC_OCP, (reg >> 1) << 16);
    return (uint16_t)rd32(REG_MAC_OCP);
}

/* PHY (MII) registers: through the PHY OCP window on 8168G and newer,
 * through PHYAR before that. */
static uint16_t phy_read(uint32_t reg) {
    if (g_family == FAM_G) {
        wr32(REG_GPHY_OCP, (0xA400u + reg * 2) << 15);
        for (int i = 0; i < 100; i++) {
            delay_us(25);
            uint32_t v = rd32(REG_GPHY_OCP);
            if (v & OCP_FLAG) return (uint16_t)v;
        }
        return 0xFFFF;
    }
    wr32(REG_PHYAR, (reg & 0x1F) << 16);
    for (int i = 0; i < 100; i++) {
        delay_us(25);
        uint32_t v = rd32(REG_PHYAR);
        if (v & OCP_FLAG) return (uint16_t)v;
    }
    return 0xFFFF;
}

static void phy_write(uint32_t reg, uint16_t v) {
    uint32_t r = g_family == FAM_G ? REG_GPHY_OCP : REG_PHYAR;
    uint32_t cmd = g_family == FAM_G ? (OCP_FLAG | ((0xA400u + reg * 2) << 15) | v)
                                     : (OCP_FLAG | ((reg & 0x1F) << 16) | v);
    wr32(r, cmd);
    for (int i = 0; i < 100; i++) {
        delay_us(25);
        if (!(rd32(r) & OCP_FLAG)) break;
    }
    delay_us(20);
}

/*
 * After UEFI the MAC can still be in "out of band" mode (owned by the
 * board's management firmware): it then never hands frames to the host.
 * Linux's rtl_hw_init_8168g takes it back. 8168G and newer only.
 */
static void take_from_firmware(void) {
    g_was_oob = (rd8(REG_MCU) & MCU_NOW_IS_OOB) != 0;
    wr32(REG_MISC, rd32(REG_MISC) | MISC_RXDV_GATE);
    for (int i = 0; i < 42 && !(rd32(REG_TX_CONFIG) & TXCFG_EMPTY); i++) delay_us(100);
    wait_set8(REG_MCU, MCU_RXTX_EMPTY, 4200);
    wr8(REG_CHIP_CMD, rd8(REG_CHIP_CMD) & ~(CMD_RX_ENABLE | CMD_TX_ENABLE));
    delay_us(1000);
    wr8(REG_MCU, rd8(REG_MCU) & ~MCU_NOW_IS_OOB);
    mac_ocp_write(0xE8DE, mac_ocp_read(0xE8DE) & ~(1u << 14));
    wait_set8(REG_MCU, MCU_LL_READY, 4200);
    mac_ocp_write(0xE8DE, mac_ocp_read(0xE8DE) | (1u << 15));
    wait_set8(REG_MCU, MCU_LL_READY, 4200);
}

/* Extended (ERI) registers, 8168G family. mask = byte enables. */
static void eri_write(uint32_t addr, uint32_t mask, uint32_t v) {
    wr32(REG_ERIDR, v);
    wr32(REG_ERIAR, OCP_FLAG | (mask << 12) | addr);
    for (int i = 0; i < 100 && (rd32(REG_ERIAR) & OCP_FLAG); i++) delay_us(10);
}

static uint32_t eri_read(uint32_t addr) {
    wr32(REG_ERIAR, (0xFu << 12) | addr);
    for (int i = 0; i < 100; i++) {
        if (rd32(REG_ERIAR) & OCP_FLAG) return rd32(REG_ERIDR);
        delay_us(10);
    }
    return 0;
}

/* Linux rtl_hw_start_8168g/h: FIFO thresholds, then a reset of the
 * receive packet filter. Without these the firmware's leftovers apply. */
static void setup_fifo_and_filter(void) {
    eri_write(0xC8, 0xF, (0x08u << 16) | 0x02u);   // rx static / dynamic
    eri_write(0xE8, 0xF, (0x10u << 16) | 0x06u);   // tx static / dynamic
    eri_write(0xCC, 0x1, 0x38);
    eri_write(0xD0, 0x1, 0x48);
    eri_write(0xDC, 0xF, eri_read(0xDC) & ~1u);    // packet filter off ...
    eri_write(0xDC, 0xF, eri_read(0xDC) | 1u);     // ... and on again
}

/* PLL and PHY may have been powered down by the firmware (no PXE, or
 * after ExitBootServices). Linux: rtl_pll_power_up + phy_resume. */
static void power_up_phy(void) {
    if (g_family == FAM_G) wr8(REG_PMCH, rd8(REG_PMCH) | 0xC0);
    else if (g_family != FAM_OLD) wr8(REG_PMCH, rd8(REG_PMCH) | 0x80);
    delay_us(1000);

    if (g_family != FAM_G) phy_write(0x1F, 0);             // standard register page
    uint16_t bmcr = phy_read(0);
    g_bmcr_at_start = bmcr;
    if (bmcr == 0xFFFF) return;                             // PHY did not answer
    bool link = rd8(REG_PHY_STATUS) & PHY_LINK_UP;
    if ((bmcr & BMCR_PDOWN) || !(bmcr & BMCR_ANENABLE) || !link) {
        // Power up and (re)start autonegotiation; the link follows in
        // about 2-4 seconds. DHCP keeps retrying meanwhile.
        bmcr = (uint16_t)((bmcr & ~BMCR_PDOWN) | BMCR_ANENABLE | BMCR_ANRESTART);
        phy_write(0, bmcr);
    }
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

    g_xid = (rd32(REG_TX_CONFIG) >> 20) & 0xFCF;
    g_family = family_of(g_xid);

    // Stop everything and reset the MAC.
    wr16(REG_INTR_MASK, 0);
    if (g_family == FAM_G) take_from_firmware();
    power_up_phy();
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
    wr32(REG_RX_CONFIG, rx_config());
    wr32(REG_TX_CONFIG, TX_CONFIG | (g_family >= FAM_EVL ? TX_AUTO_FIFO : 0));
    wr32(REG_MAR0, 0xFFFFFFFF);                            // all multicast groups
    wr32(REG_MAR0 + 4, 0xFFFFFFFF);
    wr16(REG_INTR_MASK, 0);                                // polled
    wr16(REG_INTR_STATUS, 0xFFFF);
    // Open the receive path (closed by take_from_firmware, or left closed
    // by the firmware's own driver on shutdown).
    if (g_family == FAM_G) setup_fifo_and_filter();
    if (g_family >= FAM_EVL) wr32(REG_MISC, rd32(REG_MISC) & ~MISC_RXDV_GATE);

    g_rx_next = 0;
    g_tx_next = 0;
    g_present = true;

    serial_write_str("RTL8168: initialized, XID 0x");
    serial_write_hex(g_xid);
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
    g_tx_frames++;
}

static void rtl8168_poll(void) {
    if (!g_present) return;

    // Acknowledge status (Rx OK, ring empty, FIFO overflow, ...): some
    // chips stop receiving until "Rx descriptor unavailable" is cleared.
    uint16_t status = rd16(REG_INTR_STATUS);
    g_status_seen |= status;
    if (status) wr16(REG_INTR_STATUS, status);

    // Out of step with the card: our slot is still the card's, but it has
    // filled another one. The card fills in ring order, so the oldest
    // filled slot is the first one after a card-owned run.
    if (g_rx[g_rx_next].opts1 & DESC_OWN) {
        for (uint32_t k = 1; k < RX_COUNT; k++) {
            uint32_t j = (g_rx_next + k) % RX_COUNT;
            if (g_rx[j].opts1 & DESC_OWN) continue;
            while (!(g_rx[(j + RX_COUNT - 1) % RX_COUNT].opts1 & DESC_OWN) &&
                   (j + RX_COUNT - 1) % RX_COUNT != g_rx_next)
                j = (j + RX_COUNT - 1) % RX_COUNT;
            // The card fills in order: if it has since filled our slot too,
            // we are in step after all.
            if (!(g_rx[g_rx_next].opts1 & DESC_OWN)) break;
            g_rx_next = j;
            g_rx_resyncs++;
            break;
        }
    }

    for (uint32_t budget = 0; budget < RX_COUNT; budget++) {
        uint32_t i = g_rx_next;
        uint32_t opts1 = g_rx[i].opts1;
        if (opts1 & DESC_OWN) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        uint32_t len = opts1 & DESC_LEN_MASK;                // includes the 4-byte CRC
        bool whole = (opts1 & (DESC_FIRST | DESC_LAST)) == (DESC_FIRST | DESC_LAST);
        if (whole && !(opts1 & RX_DESC_ERROR) && len > 4 + 14 && len - 4 <= ETH_FRAME_MAX) {
            g_rx_frames++;
            if (g_rx_buf[i][0] & 1) g_rx_bcast++; else g_rx_ucast++;
            eth_handle_frame(g_rx_buf[i], (uint16_t)(len - 4));
        } else {
            g_rx_errors++;
        }
        rx_give(i);
        g_rx_next = (i + 1) % RX_COUNT;
    }

    if (!g_link_reported && (rd8(REG_PHY_STATUS) & PHY_LINK_UP)) {
        serial_write_str("RTL8168: link up\n");
        g_link_reported = true;
    }
}

/* PHYstatus: bit 0 full duplex, 1 link, 2 10M, 3 100M, 4 1000M. */
static int rtl8168_link_state(uint32_t* speed, bool* full) {
    if (!g_present) return SYSCALL_NET_LINK_UNKNOWN;
    uint8_t status = rd8(REG_PHY_STATUS);
    *full = (status & 0x01) != 0;
    *speed = (status & 0x10) ? 1000u : (status & 0x08) ? 100u : (status & 0x04) ? 10u : 0u;
    return (status & PHY_LINK_UP) ? SYSCALL_NET_LINK_UP : SYSCALL_NET_LINK_DOWN;
}

// One line for the screen (the user's PC has no serial port).
static void rtl8168_describe(char* buf, uint32_t size) {
    uint8_t st = g_present ? rd8(REG_PHY_STATUS) : 0;
    uint32_t speed = (st & 0x10) ? 1000u : (st & 0x08) ? 100u : (st & 0x04) ? 10u : 0u;
    static const char* fam[] = { "8168B/C", "8168D/E", "8168E-VL/F", "8168G/H" };
    uint32_t owned = 0;
    for (uint32_t k = 0; g_present && k < RX_COUNT; k++)
        if (g_rx[k].opts1 & DESC_OWN) owned++;
    // Second line: registers that say whether the receiver is running.
    // IntrStatus bits: 1 RxOK, 2 RxErr, 4 TxOK, 10 no free Rx descriptor,
    // 40 Rx FIFO overflow.
    snprintf(buf, size,
             "RTL%s XID %03X, link %s %u Mbit, PHY BMCR %04X%s\n"
             "      rx %u (bcast %u, ucast %u, bad %u) tx %u | seen %04X cmd %02X "
             "rxcfg %08X misc %08X mcu %02X\n"
             "      ring: next %u, card-owned %u of %u, repaired %u",
             fam[g_family], g_xid, (st & PHY_LINK_UP) ? "up" : "DOWN", speed,
             g_bmcr_at_start, g_was_oob ? ", was in firmware mode" : "",
             g_rx_frames, g_rx_bcast, g_rx_ucast, g_rx_errors, g_tx_frames,
             g_status_seen, g_present ? rd8(REG_CHIP_CMD) : 0,
             g_present ? rd32(REG_RX_CONFIG) : 0, g_present ? rd32(REG_MISC) : 0,
             g_present ? rd8(REG_MCU) : 0,
             g_rx_next, owned, (uint32_t)RX_COUNT, g_rx_resyncs);
}

static const network_hw_driver_t rtl8168_driver = {
    .describe = rtl8168_describe,
    .link_state = rtl8168_link_state,
    .name    = "RTL8168",
    .init    = rtl8168_init,
    .send    = rtl8168_send,
    .poll    = rtl8168_poll,
    .get_mac = rtl8168_get_mac,
};

const network_hw_driver_t* rtl8168_get_driver(void) {
    return &rtl8168_driver;
}
