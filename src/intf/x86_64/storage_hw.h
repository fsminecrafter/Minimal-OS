#pragma once
#include <stdbool.h>

typedef struct {
    const char* name;
    bool (*init)(void);
} storage_hw_driver_t;
