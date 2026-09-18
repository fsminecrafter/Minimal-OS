#include "x86_64/network_manager.h"
#include "serial.h"

static const network_hw_driver_t* g_driver = NULL;

void network_manager_register_driver(const network_hw_driver_t* drv) {
    g_driver = drv;
}

bool network_manager_init(void) {
    if (!g_driver) return false;
    serial_write_str("network_manager: initializing driver\n");
    if (g_driver->init) return g_driver->init();
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
