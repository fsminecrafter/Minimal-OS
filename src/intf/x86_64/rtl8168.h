#pragma once
#include "x86_64/network_hw.h"

// Realtek RTL8111/8168 PCI Express gigabit Ethernet (polled).
const network_hw_driver_t* rtl8168_get_driver(void);
