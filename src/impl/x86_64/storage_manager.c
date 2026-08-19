#include "x86_64/storage_manager.h"
#include "serial.h"

static const storage_hw_driver_t* g_storage_driver = NULL;

void storage_manager_register_driver(const storage_hw_driver_t* drv) {
    g_storage_driver = drv;
}

bool storage_manager_init(void) {
    if (!g_storage_driver) return false;
    serial_write_str("storage_manager: initializing driver\n");
    if (g_storage_driver->init) return g_storage_driver->init();
    return false;
}
