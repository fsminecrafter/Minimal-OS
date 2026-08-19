#pragma once
#include <stdbool.h>
#include "x86_64/gpu_hw.h"

void gpu_manager_register_driver(const gpu_hw_driver_t* drv);
bool gpu_manager_init(void);
