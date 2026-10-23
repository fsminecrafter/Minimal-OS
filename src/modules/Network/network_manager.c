#include "x86_64/network_manager.h"
#include "serial.h"

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

void network_manager_poll(void) {
    if (g_driver && g_driver->poll) g_driver->poll();
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
