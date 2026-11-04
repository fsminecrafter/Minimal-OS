#include "x86_64/network_manager.h"
#include "serial.h"
#include "string.h"

// Drivers in registration order; the first whose hardware is present and
// starts becomes the active one.
#define NETWORK_MAX_DRIVERS 8
static const network_hw_driver_t* g_drivers[NETWORK_MAX_DRIVERS];
static uint32_t g_driver_count = 0;
static const network_hw_driver_t* g_driver = NULL;

void network_manager_register_driver(const network_hw_driver_t* drv) {
    if (drv && g_driver_count < NETWORK_MAX_DRIVERS) g_drivers[g_driver_count++] = drv;
}

bool network_manager_init(void) {
    serial_write_str("network_manager: initializing driver\n");
    for (uint32_t i = 0; i < g_driver_count; i++) {
        const network_hw_driver_t* drv = g_drivers[i];
        if (drv->init && drv->init()) {
            g_driver = drv;
            serial_write_str("network_manager: using ");
            serial_write_str(drv->name);
            serial_write_str("\n");
            return true;
        }
    }
    serial_write_str("network_manager: no supported network card\n");
    return false;
}

/*
 * The timer interrupt polls on every tick, and blocking callers (dhcp,
 * tcp, dns, ...) poll in loops on any core. Two polls at once walk the
 * NIC's receive ring together and leave it out of step with the card,
 * which then stops receiving. Only one poll runs; a poll that finds
 * another in progress (an interrupt on the same core included) returns.
 */
static volatile int g_poll_busy;

void network_manager_poll(void) {
    if (!g_driver || !g_driver->poll) return;
    if (__atomic_exchange_n(&g_poll_busy, 1, __ATOMIC_ACQUIRE)) return;
    g_driver->poll();
    __atomic_store_n(&g_poll_busy, 0, __ATOMIC_RELEASE);
}

void network_manager_send(const void* frame, uint16_t len) {
    if (g_driver && g_driver->send) g_driver->send(frame, len);
}

bool network_manager_has_driver(void) {
    return g_driver != NULL;
}

void network_manager_get_mac(uint8_t mac[6]) {
    if (g_driver && g_driver->get_mac) {
        g_driver->get_mac(mac);
    } else {
        for (int i = 0; i < 6; i++) mac[i] = 0;
    }
}

void network_manager_describe(char* buf, uint32_t size) {
    if (!size) return;
    buf[0] = 0;
    if (!g_driver) return;
    if (g_driver->describe) g_driver->describe(buf, size);
    else snprintf(buf, size, "%s", g_driver->name ? g_driver->name : "Network adapter");
}

void network_manager_adapters(syscall_net_adapters_t* out) {
    memset(out, 0, sizeof(*out));
    if (!g_driver) return;
    // Only the adapter that started is driven; it carries the address.
    syscall_net_adapter_t* a = &out->adapters[out->count++];
    const char* name = g_driver->name ? g_driver->name : "Network adapter";
    for (uint32_t i = 0; name[i] && i + 1 < sizeof(a->name); i++) a->name[i] = name[i];
    a->kind = g_driver->wireless ? SYSCALL_NET_ADAPTER_WIFI : SYSCALL_NET_ADAPTER_ETHERNET;
    a->active = 1;
    if (g_driver->get_mac) g_driver->get_mac(a->mac);
    a->link = SYSCALL_NET_LINK_UNKNOWN;
    if (g_driver->link_state) {
        bool full = false;
        a->link = (uint8_t)g_driver->link_state(&a->speed_mbps, &full);
        a->full_duplex = full ? 1 : 0;
    }
}
