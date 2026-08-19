#pragma once
#include <stdbool.h>
#include "x86_64/storage_hw.h"

void storage_manager_register_driver(const storage_hw_driver_t* drv);
bool storage_manager_init(void);
