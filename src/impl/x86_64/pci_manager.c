#include "x86_64/pci_manager.h"
#include "serial.h"

#define MAX_CLASS_DRIVERS 16
static const pci_class_driver_t* g_class_drivers[MAX_CLASS_DRIVERS];
static int g_class_driver_count = 0;

void pci_manager_register_class_driver(const pci_class_driver_t* drv) {
    if (g_class_driver_count >= MAX_CLASS_DRIVERS) return;
    g_class_drivers[g_class_driver_count++] = drv;
}

void pci_manager_notify(pci_device_t* dev) {
    for (int i = 0; i < g_class_driver_count; i++) {
        const pci_class_driver_t* d = g_class_drivers[i];
        if (d->class_code == dev->class_code && d->subclass == dev->subclass) {
            if (d->notify) d->notify(dev);
        }
    }
}
