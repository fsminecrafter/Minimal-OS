#pragma once
#include <stdbool.h>
#include "x86_64/usb_hw.h"

void usb_manager_register_driver(const usb_hw_driver_t* drv);
bool usb_manager_init(void);
