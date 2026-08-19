#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/pci.h"

typedef struct {
    uint8_t class_code;
    uint8_t subclass;
    void (*notify)(pci_device_t* dev);
} pci_class_driver_t;

void pci_manager_register_class_driver(const pci_class_driver_t* drv);
void pci_manager_notify(pci_device_t* dev);
