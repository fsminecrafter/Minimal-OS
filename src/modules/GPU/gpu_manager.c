#include "x86_64/gpu_manager.h"
#include "serial.h"

#define GPU_MANAGER_MAX_DRIVERS 4

static const gpu_hw_driver_t* g_gpu_drivers[GPU_MANAGER_MAX_DRIVERS];
static size_t g_gpu_driver_count;
static const gpu_hw_driver_t* g_active_gpu_driver;

void gpu_manager_register_driver(const gpu_hw_driver_t* drv) {
    if (!drv || g_active_gpu_driver) return;
    for (size_t i = 0; i < g_gpu_driver_count; i++) {
        if (g_gpu_drivers[i] == drv) return;
    }
    if (g_gpu_driver_count < GPU_MANAGER_MAX_DRIVERS) {
        g_gpu_drivers[g_gpu_driver_count++] = drv;
    }
}

bool gpu_manager_init(void) {
    if (g_active_gpu_driver) return true;
    for (size_t i = 0; i < g_gpu_driver_count; i++) {
        const gpu_hw_driver_t* driver = g_gpu_drivers[i];
        if (!driver || !driver->init) continue;
        serial_write_str("gpu_manager: trying ");
        serial_write_str(driver->name ? driver->name : "unnamed driver");
        serial_write_str("\n");
        if (driver->init()) {
            g_active_gpu_driver = driver;
            serial_write_str("gpu_manager: active driver: ");
            serial_write_str(driver->name ? driver->name : "unnamed driver");
            serial_write_str("\n");
            return true;
        }
    }
    return false;
}
