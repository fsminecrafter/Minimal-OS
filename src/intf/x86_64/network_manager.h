#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/network_hw.h"

void network_manager_register_driver(const network_hw_driver_t* drv);
bool network_manager_init(void);
void network_manager_poll(void);
void network_manager_send(const void* frame, uint16_t len);
bool network_manager_has_driver(void);
void network_manager_get_mac(uint8_t mac[6]);
