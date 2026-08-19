#include "x86_64/usb_manager.h"
#include "serial.h"

static const usb_hw_driver_t* g_usb_driver = NULL;

void usb_manager_register_driver(const usb_hw_driver_t* drv) {
    g_usb_driver = drv;
}

bool usb_manager_init(void) {
    if (!g_usb_driver) return false;
    serial_write_str("usb_manager: initializing driver\n");
    if (g_usb_driver->init) return g_usb_driver->init();
    return false;
}
