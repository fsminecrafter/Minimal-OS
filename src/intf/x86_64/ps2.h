#pragma once
#include <stdint.h>
#include <stdbool.h>

uint8_t ps2_read_scan_code();

// Sets up the 8042 controller and enables the keyboard. Call with
// interrupts disabled, before relying on IRQ1.
bool ps2_controller_init(void);
