#ifndef UHCI_H
#define UHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "usb/usb_stack.h"
#include "x86_64/pci.h"

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

#endif // UHCI_H