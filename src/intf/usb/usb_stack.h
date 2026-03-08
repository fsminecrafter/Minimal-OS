#ifndef USB_STACK_H
#define USB_STACK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * MINIMAL USB STACK FOR USB KEYBOARD SUPPORT
 * 
 * This provides a minimal USB host controller implementation
 * focused on USB HID keyboard support.
 */

// ===========================================
// USB DEVICE STATES
// ===========================================

typedef enum {
    USB_DEVICE_STATE_DETACHED = 0,
    USB_DEVICE_STATE_ATTACHED,
    USB_DEVICE_STATE_POWERED,
    USB_DEVICE_STATE_DEFAULT,
    USB_DEVICE_STATE_ADDRESS,
    USB_DEVICE_STATE_CONFIGURED,
} usb_device_state_t;

// ===========================================
// USB DESCRIPTORS
// ===========================================

// Device descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

// Configuration descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_descriptor_t;

// Interface descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

// Endpoint descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

// HID descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bDescriptorType2;
    uint16_t wDescriptorLength;
} __attribute__((packed)) usb_hid_descriptor_t;

// ===========================================
// USB DEVICE
// ===========================================

#define USB_MAX_ENDPOINTS 16

typedef struct {
    uint8_t address;
    uint8_t port;
    usb_device_state_t state;
    uint8_t config_value;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_s;
    uint8_t subclass;
    uint8_t protocol;
    bool is_keyboard;
    uint8_t keyboard_endpoint;
    uint16_t keyboard_max_packet_size;
    uint8_t keyboard_interval;
} usb_device_t;

// ===========================================
// USB HOST CONTROLLER (UHCI/EHCI/XHCI)
// ===========================================

typedef enum {
    USB_HC_TYPE_NONE = 0,
    USB_HC_TYPE_UHCI,   // USB 1.1
    USB_HC_TYPE_EHCI,   // USB 2.0
    USB_HC_TYPE_XHCI,   // USB 3.0+
} usb_hc_type_t;

typedef struct {
    usb_hc_type_t type;
    uint64_t mmio_base;
    uint16_t io_base;
    uint8_t irq;
    bool initialized;
    usb_device_t devices[8];  // Up to 8 devices
    uint8_t num_devices;
} usb_host_controller_t;

// ===========================================
// USB STACK FUNCTIONS
// ===========================================

// Initialize USB subsystem
bool usb_init(void);

// Detect and enumerate USB devices
void usb_enumerate_devices(void);

// Poll for USB events (call periodically)
void usb_poll(void);

// Get keyboard device (if any)
usb_device_t* usb_get_keyboard(void);

// Read from USB keyboard endpoint
bool usb_keyboard_read(usb_device_t* dev, uint8_t* buffer, uint8_t size);

// ===========================================
// MINIMAL IMPLEMENTATION NOTES
// ===========================================

/*
For a minimal implementation, you need:

1. PCI ENUMERATION:
   - Scan PCI bus for USB host controllers
   - Look for class 0x0C (Serial Bus), subclass 0x03 (USB)
   - Interface: 0x00 (UHCI), 0x20 (EHCI), 0x30 (XHCI)

2. HOST CONTROLLER INIT:
   - Map MMIO/IO registers
   - Reset controller
   - Enable interrupts
   - Set up root hub

3. DEVICE DETECTION:
   - Detect device attachment
   - Reset port
   - Get device descriptor
   - Assign address
   - Get configuration
   - Set configuration

4. HID KEYBOARD SETUP:
   - Check interface class (0x03 = HID)
   - Check interface protocol (0x01 = Keyboard)
   - Find interrupt IN endpoint
   - Set up periodic polling (interrupt transfer)

5. KEYBOARD INPUT:
   - Poll interrupt endpoint (every ~10ms)
   - Read 8-byte HID report
   - Pass to usb_keyboard_process_report()
*/

#endif // USB_STACK_H