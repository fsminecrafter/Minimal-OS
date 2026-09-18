#include <stdint.h>
#include <stdbool.h>
#include "x86_64/rtl8139.h"
#include "x86_64/pci.h"
#include "x86_64/port.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "serial.h"
#include "string.h"
#include "net/ethernet.h"

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

#define REG_IDR0     0x00
#define REG_TSD0     0x10
#define REG_TSAD0    0x20
#define REG_RBSTART  0x30
#define REG_CAPR     0x38
#define REG_CMD      0x37
#define REG_IMR      0x3C
#define REG_RCR      0x44
#define REG_TCR      0x40
#define REG_CONFIG1  0x52

#define CMD_RESET    0x10
#define CMD_RX_EN    0x08
#define CMD_TX_EN    0x04
#define CMD_BUFE     0x01

#define RCR_AAP  (1 << 0)
#define RCR_APM  (1 << 1)
#define RCR_AM   (1 << 2)
#define RCR_AB   (1 << 3)
#define RCR_WRAP (1 << 7)

#define RX_BUFFER_SIZE (8192 + 16 + 1500)
#define TX_BUFFER_SIZE 1536
#define NUM_TX_DESC 4

static uint16_t g_io_base = 0;
static uint8_t* g_rx_buffer = NULL;
static uint8_t* g_tx_buffer[NUM_TX_DESC];
static uint32_t g_rx_offset = 0;
static uint32_t g_tx_next = 0;
static uint8_t g_mac[6];
static bool g_present = false;

static inline void wr8(uint16_t reg, uint8_t v)   { port_outb(g_io_base + reg, v); }
static inline void wr16(uint16_t reg, uint16_t v) { port_outw(g_io_base + reg, v); }
static inline void wr32(uint16_t reg, uint32_t v) { port_outl(g_io_base + reg, v); }
static inline uint8_t  rd8(uint16_t reg)  { return port_inb(g_io_base + reg); }
static inline uint32_t rd32(uint16_t reg) { return port_inl(g_io_base + reg); }

static pci_device_t* find_rtl8139(void) {
    extern pci_device_t pci_devices[];
    extern int pci_device_count;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == RTL8139_VENDOR_ID &&
            pci_devices[i].device_id == RTL8139_DEVICE_ID) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

static bool rtl8139_init(void) {
    pci_device_t* dev = find_rtl8139();
    if (!dev) {
        serial_write_str("RTL8139: not found\n");
        return false;
    }

    pci_enable_io_busmaster(dev);

    if (dev->bar_type[0] != PCI_BAR_IO) {
        serial_write_str("RTL8139: BAR0 is not I/O space\n");
        return false;
    }
    g_io_base = (uint16_t)dev->bar[0];

    wr8(REG_CONFIG1, 0x00);

    wr8(REG_CMD, CMD_RESET);
    int timeout = 1000000;
    while ((rd8(REG_CMD) & CMD_RESET) && timeout-- > 0) { }
    if (timeout <= 0) {
        serial_write_str("RTL8139: reset timeout\n");
        return false;
    }

    for (int i = 0; i < 6; i++) g_mac[i] = port_inb(g_io_base + REG_IDR0 + i);

    // DMA-safe physically contiguous buffers. This kernel identity-maps
    // low RAM, so the virtual pointer from the PMM IS the physical
    // address the NIC needs to be told about.
    uint32_t rx_pages = (RX_BUFFER_SIZE + 0xFFF) / 0x1000;
    g_rx_buffer = (uint8_t*)alloc_pages_zeroed(rx_pages);
    if (!g_rx_buffer) {
        serial_write_str("RTL8139: OOM allocating RX buffer\n");
        return false;
    }
    wr32(REG_RBSTART, (uint32_t)(uintptr_t)g_rx_buffer);

    for (int i = 0; i < NUM_TX_DESC; i++) {
        g_tx_buffer[i] = (uint8_t*)alloc_page_zeroed();
        if (!g_tx_buffer[i]) {
            serial_write_str("RTL8139: OOM allocating TX buffer\n");
            return false;
        }
    }

    wr8(REG_CMD, CMD_RX_EN | CMD_TX_EN);

    // Accept everything and filter in software - simplest correct option.
    wr32(REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);
    wr32(REG_TCR, 0x03000700);
    wr16(REG_IMR, 0x0000);  // polled, no IRQs

    g_rx_offset = 0;
    g_tx_next = 0;
    g_present = true;

    serial_write_str("RTL8139: initialized, MAC=");
    for (int i = 0; i < 6; i++) {
        serial_write_hex(g_mac[i]);
        if (i < 5) serial_write_str(":");
    }
    serial_write_str("\n");
    return true;
}

static void rtl8139_get_mac(uint8_t out[6]) {
    memcpy(out, g_mac, 6);
}

static void rtl8139_send(const void* frame, uint16_t len) {
    if (!g_present) return;
    if (len > TX_BUFFER_SIZE) len = TX_BUFFER_SIZE;

    uint32_t slot = g_tx_next;
    g_tx_next = (g_tx_next + 1) % NUM_TX_DESC;

    // Bounded wait for OWN (bit13) - set once the NIC has consumed this
    // descriptor's previous contents and it's free to reuse.
    int timeout = 100000;
    while (!(rd32(REG_TSD0 + slot * 4) & (1 << 13)) && timeout-- > 0) {
        asm volatile("pause");
    }

    memcpy(g_tx_buffer[slot], frame, len);
    if (len < 60) {
        memset(g_tx_buffer[slot] + len, 0, 60 - len);
        len = 60; // minimum Ethernet frame size
    }

    wr32(REG_TSAD0 + slot * 4, (uint32_t)(uintptr_t)g_tx_buffer[slot]);
    wr32(REG_TSD0 + slot * 4, len); // clears OWN, starts transmission
}

static void rtl8139_poll(void) {
    if (!g_present) return;

    while (!(rd8(REG_CMD) & CMD_BUFE)) {
        uint8_t* header_ptr = g_rx_buffer + g_rx_offset;
        uint16_t status = *(uint16_t*)(header_ptr + 0);
        uint16_t length  = *(uint16_t*)(header_ptr + 2);

        if (!(status & 0x01) || length < 14 || length > RX_BUFFER_SIZE) {
            serial_write_str("RTL8139: bad rx status, resetting ring\n");
            g_rx_offset = 0;
            port_outw(g_io_base + REG_CAPR, (uint16_t)(g_rx_offset - 16));
            break;
        }

        uint8_t* packet = header_ptr + 4;
        static uint8_t linear[ETH_FRAME_MAX];
        uint16_t payload_len = length - 4; // exclude trailing CRC

        if (g_rx_offset + 4 + payload_len <= RX_BUFFER_SIZE) {
            memcpy(linear, packet, payload_len);
        } else {
            uint32_t first_part = RX_BUFFER_SIZE - (g_rx_offset + 4);
            memcpy(linear, packet, first_part);
            memcpy(linear + first_part, g_rx_buffer, payload_len - first_part);
        }

        eth_handle_frame(linear, payload_len);

        g_rx_offset = (g_rx_offset + length + 4 + 3) & ~3u;
        if (g_rx_offset >= RX_BUFFER_SIZE) g_rx_offset -= RX_BUFFER_SIZE;
        port_outw(g_io_base + REG_CAPR, (uint16_t)(g_rx_offset - 16));
    }
}

static const network_hw_driver_t rtl8139_driver = {
    .name    = "RTL8139",
    .init    = rtl8139_init,
    .send    = rtl8139_send,
    .poll    = rtl8139_poll,
    .get_mac = rtl8139_get_mac,
};

const network_hw_driver_t* rtl8139_get_driver(void) {
    return &rtl8139_driver;
}
