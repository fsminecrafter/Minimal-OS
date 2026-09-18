#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char* name;
    bool (*init)(void);
    void (*send)(const void* frame, uint16_t len);
    void (*poll)(void);                 // pump rx ring; calls eth_handle_frame() per frame
    void (*get_mac)(uint8_t mac[6]);
} network_hw_driver_t;
