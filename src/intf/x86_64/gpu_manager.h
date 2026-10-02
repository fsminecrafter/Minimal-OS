#pragma once
#include <stdbool.h>
#include "x86_64/gpu_hw.h"
#include "x86_64/multiboot2parse.h"

void gpu_manager_register_driver(const gpu_hw_driver_t* drv);
bool gpu_manager_init(void);
void gpu_multiboot2_set_info(multiboot2_info_t* info);
const gpu_hw_driver_t* gpu_multiboot2_get_driver(void);
