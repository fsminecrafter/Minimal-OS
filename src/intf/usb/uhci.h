#ifndef UHCI_H
#define UHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "usb/usb_stack.h"
#include "x86_64/pci.h"
#include "x86_64/usb_hw.h"

// UHCI-specific functions
bool uhci_init(pci_device_t* dev);
bool uhci_control_transfer(uint8_t dev_addr, usb_setup_packet_t* setup, void* data, uint16_t length);
bool uhci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint, void* buffer, uint16_t length, bool low_speed);

// Device enumeration
bool usb_enumerate_device(uint8_t port, bool low_speed);
bool usb_parse_configuration(usb_device_t* dev, uint8_t* config_data, uint16_t length);
bool usb_keyboard_init_device(usb_device_t* dev);

// Keyboard polling
void usb_poll_keyboard(usb_device_t* dev);
void uhci_keyboard_interrupt_init(usb_device_t* dev);

// Returns the usb_hw_driver_t vtable for registration with
// usb_manager_register_driver(). Prefer this + usb_manager_init()
// over calling usb_init() directly by name.
const usb_hw_driver_t* uhci_get_driver(void);

#endif // UHCI_H
