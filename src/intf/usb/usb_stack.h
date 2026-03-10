#ifndef USB_STACK_H
#define USB_STACK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * USB STACK FOR USB KEYBOARD SUPPORT
 * 
 * Complete USB host controller implementation with UHCI support
 * and HID keyboard handling.
 */

// ===========================================
// USB STANDARD CONSTANTS
// ===========================================

// USB Request Types
#define USB_REQ_DIR_OUT           0x00
#define USB_REQ_DIR_IN            0x80
#define USB_REQ_TYPE_STANDARD     0x00
#define USB_REQ_TYPE_CLASS        0x20
#define USB_REQ_TYPE_VENDOR       0x40
#define USB_REQ_RECIPIENT_DEVICE  0x00
#define USB_REQ_RECIPIENT_INTERFACE 0x01
#define USB_REQ_RECIPIENT_ENDPOINT 0x02

// USB Standard Requests
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SYNCH_FRAME       0x0C

// USB Descriptor Types
#define USB_DESC_DEVICE           0x01
#define USB_DESC_CONFIGURATION    0x02
#define USB_DESC_STRING           0x03
#define USB_DESC_INTERFACE        0x04
#define USB_DESC_ENDPOINT         0x05
#define USB_DESC_DEVICE_QUALIFIER 0x06
#define USB_DESC_HID              0x21
#define USB_DESC_REPORT           0x22
#define USB_DESC_PHYSICAL         0x23

// USB Classes
#define USB_CLASS_HID             0x03
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_CLASS_HUB             0x09

// HID Subclasses
#define USB_HID_SUBCLASS_NONE     0x00
#define USB_HID_SUBCLASS_BOOT     0x01

// HID Protocols
#define USB_HID_PROTOCOL_NONE     0x00
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE    0x02

// HID Class Requests
#define USB_HID_REQ_GET_REPORT    0x01
#define USB_HID_REQ_GET_IDLE      0x02
#define USB_HID_REQ_GET_PROTOCOL  0x03
#define USB_HID_REQ_SET_REPORT    0x09
#define USB_HID_REQ_SET_IDLE      0x0A
#define USB_HID_REQ_SET_PROTOCOL  0x0B

// Endpoint Types
#define USB_ENDPOINT_TYPE_CONTROL     0x00
#define USB_ENDPOINT_TYPE_ISOCHRONOUS 0x01
#define USB_ENDPOINT_TYPE_BULK        0x02
#define USB_ENDPOINT_TYPE_INTERRUPT   0x03

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
    USB_DEVICE_STATE_SUSPENDED,
} usb_device_state_t;

// ===========================================
// USB SETUP PACKET (CRITICAL!)
// ===========================================

/**
 * USB Setup Packet - 8 bytes
 * Used for all control transfers
 */
typedef struct {
    uint8_t  bmRequestType;  // Request type (direction, type, recipient)
    uint8_t  bRequest;       // Request code
    uint16_t wValue;         // Request-specific value
    uint16_t wIndex;         // Request-specific index (e.g., interface number)
    uint16_t wLength;        // Number of bytes to transfer in data stage
} __attribute__((packed)) usb_setup_packet_t;

// ===========================================
// USB DESCRIPTORS
// ===========================================

/**
 * Device Descriptor - 18 bytes
 * Describes the entire device
 */
typedef struct {
    uint8_t  bLength;            // Size of this descriptor (18)
    uint8_t  bDescriptorType;    // DEVICE descriptor type (0x01)
    uint16_t bcdUSB;             // USB specification release number (BCD)
    uint8_t  bDeviceClass;       // Class code (0 = defined in interface)
    uint8_t  bDeviceSubClass;    // Subclass code
    uint8_t  bDeviceProtocol;    // Protocol code
    uint8_t  bMaxPacketSize0;    // Max packet size for endpoint 0 (8, 16, 32, 64)
    uint16_t idVendor;           // Vendor ID
    uint16_t idProduct;          // Product ID
    uint16_t bcdDevice;          // Device release number (BCD)
    uint8_t  iManufacturer;      // Index of manufacturer string
    uint8_t  iProduct;           // Index of product string
    uint8_t  iSerialNumber;      // Index of serial number string
    uint8_t  bNumConfigurations; // Number of possible configurations
} __attribute__((packed)) usb_device_descriptor_t;

/**
 * Configuration Descriptor - 9 bytes
 * Describes one device configuration
 */
typedef struct {
    uint8_t  bLength;            // Size of this descriptor (9)
    uint8_t  bDescriptorType;    // CONFIGURATION descriptor type (0x02)
    uint16_t wTotalLength;       // Total length of data (config + interfaces + endpoints)
    uint8_t  bNumInterfaces;     // Number of interfaces in this configuration
    uint8_t  bConfigurationValue;// Configuration value (used in SET_CONFIGURATION)
    uint8_t  iConfiguration;     // Index of configuration string
    uint8_t  bmAttributes;       // Configuration characteristics (bit 7 must be 1)
    uint8_t  bMaxPower;          // Maximum power consumption in 2mA units
} __attribute__((packed)) usb_config_descriptor_t;

/**
 * Interface Descriptor - 9 bytes
 * Describes one interface within a configuration
 */
typedef struct {
    uint8_t bLength;            // Size of this descriptor (9)
    uint8_t bDescriptorType;    // INTERFACE descriptor type (0x04)
    uint8_t bInterfaceNumber;   // Interface number (0-based)
    uint8_t bAlternateSetting;  // Alternate setting number
    uint8_t bNumEndpoints;      // Number of endpoints (excluding endpoint 0)
    uint8_t bInterfaceClass;    // Class code (0x03 = HID)
    uint8_t bInterfaceSubClass; // Subclass code (0x01 = Boot Interface)
    uint8_t bInterfaceProtocol; // Protocol code (0x01 = Keyboard, 0x02 = Mouse)
    uint8_t iInterface;         // Index of interface string
} __attribute__((packed)) usb_interface_descriptor_t;

/**
 * Endpoint Descriptor - 7 bytes
 * Describes one endpoint
 */
typedef struct {
    uint8_t  bLength;           // Size of this descriptor (7)
    uint8_t  bDescriptorType;   // ENDPOINT descriptor type (0x05)
    uint8_t  bEndpointAddress;  // Endpoint address (bit 7: direction, bits 3-0: number)
    uint8_t  bmAttributes;      // Endpoint attributes (bits 1-0: transfer type)
    uint16_t wMaxPacketSize;    // Maximum packet size
    uint8_t  bInterval;         // Polling interval (in frames/microframes)
} __attribute__((packed)) usb_endpoint_descriptor_t;

/**
 * HID Descriptor - 9 bytes minimum
 * Describes HID class-specific information
 */
typedef struct {
    uint8_t  bLength;           // Size of this descriptor
    uint8_t  bDescriptorType;   // HID descriptor type (0x21)
    uint16_t bcdHID;            // HID specification release number (BCD)
    uint8_t  bCountryCode;      // Country code (0 = not localized)
    uint8_t  bNumDescriptors;   // Number of class descriptors
    uint8_t  bDescriptorType2;  // Report descriptor type (0x22)
    uint16_t wDescriptorLength; // Report descriptor length
} __attribute__((packed)) usb_hid_descriptor_t;

/**
 * String Descriptor
 * Contains UTF-16LE encoded strings
 */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wData[];  // UTF-16LE encoded string
} __attribute__((packed)) usb_string_descriptor_t;

// ===========================================
// USB DEVICE
// ===========================================

#define USB_MAX_ENDPOINTS 16

typedef struct {
    // Device identification
    uint8_t address;            // Device address (1-127, 0 = default)
    uint8_t port;               // Port number on root hub
    usb_device_state_t state;   // Current device state
    
    // Configuration
    uint8_t config_value;       // Active configuration value
    uint8_t max_packet_size;    // Max packet size for endpoint 0
    
    // Device information
    uint16_t vendor_id;         // Vendor ID
    uint16_t product_id;        // Product ID
    uint8_t  class_code;        // Device class code
    uint8_t  subclass;          // Device subclass
    uint8_t  protocol;          // Device protocol
    
    // Speed detection
    bool low_speed;             // True if low-speed (1.5 Mbps)
    bool full_speed;            // True if full-speed (12 Mbps)
    
    // Keyboard-specific fields (when is_keyboard = true)
    bool is_keyboard;           // True if this is a HID keyboard
    uint8_t keyboard_endpoint;  // Interrupt IN endpoint number
    uint16_t keyboard_max_packet_size;  // Keyboard endpoint max packet
    uint8_t keyboard_interval;  // Keyboard polling interval (ms)
    
    // Mouse-specific fields (future expansion)
    bool is_mouse;
    uint8_t mouse_endpoint;
    
    // Timing
    uint64_t last_poll_time;    // Last time device was polled
} usb_device_t;

// ===========================================
// USB HOST CONTROLLER
// ===========================================

typedef enum {
    USB_HC_TYPE_NONE = 0,
    USB_HC_TYPE_UHCI,   // USB 1.1 (Intel/VIA) - I/O port based
    USB_HC_TYPE_OHCI,   // USB 1.1 (Compaq/Microsoft) - MMIO based
    USB_HC_TYPE_EHCI,   // USB 2.0
    USB_HC_TYPE_XHCI,   // USB 3.0+
} usb_hc_type_t;

typedef struct {
    usb_hc_type_t type;         // Controller type
    uint64_t mmio_base;         // MMIO base address (for EHCI/XHCI/OHCI)
    uint16_t io_base;           // I/O base address (for UHCI)
    uint8_t irq;                // IRQ number
    bool initialized;           // True if successfully initialized
    
    // Root hub ports
    uint8_t num_ports;          // Number of root hub ports (usually 2-8)
    
    // Connected devices
    usb_device_t devices[8];    // Up to 8 devices
    uint8_t num_devices;        // Number of enumerated devices
} usb_host_controller_t;

// ===========================================
// USB TRANSFER TYPES & RESULTS
// ===========================================

typedef enum {
    USB_TRANSFER_CONTROL = 0,
    USB_TRANSFER_INTERRUPT,
    USB_TRANSFER_BULK,
    USB_TRANSFER_ISOCHRONOUS,
} usb_transfer_type_t;

typedef enum {
    USB_TRANSFER_SUCCESS = 0,
    USB_TRANSFER_ERROR,
    USB_TRANSFER_TIMEOUT,
    USB_TRANSFER_STALL,
    USB_TRANSFER_NAK,
    USB_TRANSFER_BABBLE,
    USB_TRANSFER_DATA_BUFFER_ERROR,
} usb_transfer_result_t;

// ===========================================
// USB STACK FUNCTIONS
// ===========================================

/**
 * Initialize USB subsystem
 * - Scans PCI for USB controllers
 * - Initializes found controller
 * - Enumerates devices
 * @return true if USB controller found and initialized
 */
bool usb_init(void);

/**
 * Shutdown USB subsystem
 */
void usb_shutdown(void);

/**
 * Detect and enumerate all USB devices
 * Called automatically by usb_init()
 */
void usb_enumerate_devices(void);

/**
 * Poll for USB events
 * Call this periodically (e.g., every 10ms from timer interrupt)
 * This will poll keyboards for input
 */
void usb_poll(void);

/**
 * Get first USB keyboard device
 * @return pointer to keyboard device, or NULL if none found
 */
usb_device_t* usb_get_keyboard(void);

/**
 * Read from USB keyboard interrupt endpoint
 * @param dev USB device (must be keyboard)
 * @param buffer Buffer to receive 8-byte HID report
 * @param size Buffer size (must be >= 8)
 * @return true if data was read successfully
 */
bool usb_keyboard_read(usb_device_t* dev, uint8_t* buffer, uint8_t size);

/**
 * Find device by class code
 * @param class_code USB class code (e.g., 0x03 for HID)
 * @return pointer to device, or NULL if not found
 */
usb_device_t* usb_find_device_by_class(uint8_t class_code);

// ===========================================
// DEBUG & UTILITY FUNCTIONS
// ===========================================

/**
 * Get human-readable name for USB class
 */
const char* usb_get_device_class_name(uint8_t class_code);

/**
 * Get human-readable name for transfer type
 */
const char* usb_get_transfer_type_name(usb_transfer_type_t type);

/**
 * Get human-readable name for controller type
 */
const char* usb_get_controller_type_name(usb_hc_type_t type);

/**
 * Print device descriptor to serial output
 */
void usb_print_device_descriptor(usb_device_descriptor_t* desc);

/**
 * Print configuration descriptor to serial output
 */
void usb_print_config_descriptor(usb_config_descriptor_t* desc);

/**
 * Print interface descriptor to serial output
 */
void usb_print_interface_descriptor(usb_interface_descriptor_t* desc);

/**
 * Print endpoint descriptor to serial output
 */
void usb_print_endpoint_descriptor(usb_endpoint_descriptor_t* desc);

// ===========================================
// HELPER MACROS
// ===========================================

// Extract endpoint information
#define USB_ENDPOINT_DIR(addr)      ((addr) & 0x80)
#define USB_ENDPOINT_NUM(addr)      ((addr) & 0x0F)
#define USB_ENDPOINT_IS_IN(addr)    (((addr) & 0x80) != 0)
#define USB_ENDPOINT_IS_OUT(addr)   (((addr) & 0x80) == 0)

// Build request type byte
#define USB_REQUEST_TYPE(dir, type, recipient) \
    ((dir) | (type) | (recipient))

// Common request types for standard requests
#define USB_RT_DEV_TO_HOST  USB_REQUEST_TYPE(USB_REQ_DIR_IN, USB_REQ_TYPE_STANDARD, USB_REQ_RECIPIENT_DEVICE)
#define USB_RT_HOST_TO_DEV  USB_REQUEST_TYPE(USB_REQ_DIR_OUT, USB_REQ_TYPE_STANDARD, USB_REQ_RECIPIENT_DEVICE)
#define USB_RT_IF_TO_HOST   USB_REQUEST_TYPE(USB_REQ_DIR_IN, USB_REQ_TYPE_STANDARD, USB_REQ_RECIPIENT_INTERFACE)
#define USB_RT_HOST_TO_IF   USB_REQUEST_TYPE(USB_REQ_DIR_OUT, USB_REQ_TYPE_STANDARD, USB_REQ_RECIPIENT_INTERFACE)
#define USB_RT_EP_TO_HOST   USB_REQUEST_TYPE(USB_REQ_DIR_IN, USB_REQ_TYPE_STANDARD, USB_REQ_RECIPIENT_ENDPOINT)
#define USB_RT_HOST_TO_EP   USB_REQUEST_TYPE(USB_REQ_DIR_OUT, USB_REQ_TYPE_STANDARD, USB_REQ_RECIPIENT_ENDPOINT)

// Common request types for HID class requests
#define USB_RT_HID_TO_HOST  USB_REQUEST_TYPE(USB_REQ_DIR_IN, USB_REQ_TYPE_CLASS, USB_REQ_RECIPIENT_INTERFACE)
#define USB_RT_HOST_TO_HID  USB_REQUEST_TYPE(USB_REQ_DIR_OUT, USB_REQ_TYPE_CLASS, USB_REQ_RECIPIENT_INTERFACE)

// ===========================================
// INLINE HELPER FUNCTIONS
// ===========================================

/**
 * Check if device is a keyboard
 */
static inline bool usb_is_keyboard(usb_device_t* dev) {
    return dev && dev->is_keyboard;
}

/**
 * Check if device is configured and ready to use
 */
static inline bool usb_is_configured(usb_device_t* dev) {
    return dev && dev->state == USB_DEVICE_STATE_CONFIGURED;
}

/**
 * Get device speed as human-readable string
 */
static inline const char* usb_get_speed_string(usb_device_t* dev) {
    if (!dev) return "unknown";
    if (dev->low_speed) return "low-speed (1.5 Mbps)";
    if (dev->full_speed) return "full-speed (12 Mbps)";
    return "high-speed (480 Mbps)";
}

/**
 * Check if device is low-speed
 */
static inline bool usb_is_low_speed(usb_device_t* dev) {
    return dev && dev->low_speed;
}

/**
 * Check if device is full-speed
 */
static inline bool usb_is_full_speed(usb_device_t* dev) {
    return dev && dev->full_speed;
}

// ===========================================
// IMPLEMENTATION NOTES
// ===========================================

/*
MINIMAL IMPLEMENTATION FLOW:

1. PCI ENUMERATION (pci.c):
   - Scan for class 0x0C, subclass 0x03 (USB controller)
   - Check prog_if: 0x00 = UHCI, 0x20 = EHCI, 0x30 = XHCI
   
2. CONTROLLER INITIALIZATION (usb_uhci.c):
   - Get I/O base from BAR4 (UHCI) or MMIO base (EHCI/XHCI)
   - Reset controller
   - Set up frame list (1024 pointers, 4KB aligned)
   - Create queue heads for control/interrupt/bulk
   - Start controller
   
3. DEVICE ENUMERATION (usb_enumeration.c):
   - Check port status for connected devices
   - Reset port (50ms reset + 10ms recovery)
   - Get device descriptor (first 8 bytes to get max packet size)
   - Set address (assign unique address 1-127)
   - Get full device descriptor
   - Get configuration descriptor
   - Parse for HID keyboard (class 0x03, protocol 0x01)
   - Set configuration
   - Set boot protocol (for keyboards)
   
4. KEYBOARD POLLING (usb_interrupt.c):
   - Set up interrupt transfer on keyboard endpoint
   - Poll every 10ms (or use keyboard's bInterval)
   - Read 8-byte HID report
   - Pass to usb_keyboard_process_report()
   
5. INTEGRATION:
   - Call usb_poll() from timer interrupt or main loop
   - USB keyboard driver processes reports
   - Terminal receives keyboard events

KEY TIMING REQUIREMENTS:
- Port reset: 50ms minimum
- Reset recovery: 10ms minimum
- Address assignment: 2ms recovery time
- Control transfer timeout: 5 seconds max
- Interrupt polling: 1-255ms (keyboard usually 10ms)

ERROR HANDLING:
- Check TD status for STALL, BABBLE, CRC/Timeout, etc.
- Retry control transfers on timeout (max 3 times)
- Ignore NAK on interrupt transfers (no data available)
- Reset device on repeated errors
*/

#endif // USB_STACK_H