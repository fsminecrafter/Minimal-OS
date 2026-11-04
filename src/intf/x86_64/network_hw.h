#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char* name;
    bool (*init)(void);
    void (*send)(const void* frame, uint16_t len);
    void (*poll)(void);                 // pump rx ring; calls eth_handle_frame() per frame
    void (*get_mac)(uint8_t mac[6]);
    // Optional: SYSCALL_NET_LINK_* now, link speed in Mbit/s (0 unknown).
    int (*link_state)(uint32_t* speed_mbps, bool* full_duplex);
    // Optional: one-line diagnostics (chip, link, counters) for the screen.
    void (*describe)(char* buf, uint32_t size);
    uint8_t wireless;                   // 1 for Wi-Fi adapters
} network_hw_driver_t;
