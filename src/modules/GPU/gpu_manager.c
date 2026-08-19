#include "x86_64/gpu_manager.h"
#include "serial.h"

static const gpu_hw_driver_t* g_gpu_driver = NULL;

void gpu_manager_register_driver(const gpu_hw_driver_t* drv) {
    g_gpu_driver = drv;
}

bool gpu_manager_init(void) {
    if (!g_gpu_driver) return false;
    serial_write_str("gpu_manager: initializing driver\n");
    if (g_gpu_driver->init) return g_gpu_driver->init();
    return false;
}
