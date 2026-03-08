#include "usb/usb_stack.h"
#include "keyboard/usbkeyboard.h"
#include "serial.h"
#include "string.h"

// ===========================================
// GLOBAL USB STATE
// ===========================================

static usb_host_controller_t g_usb_hc = {0};
static bool g_usb_initialized = false;

// ===========================================
// USB INITIALIZATION
// ===========================================

bool usb_init(void) {
    serial_write_str("USB: Initializing USB stack...\n");
    
    // TODO: Scan PCI for USB controllers
    // For now, we'll stub this out
    
    g_usb_hc.type = USB_HC_TYPE_NONE;
    g_usb_hc.initialized = false;
    g_usb_hc.num_devices = 0;
    
    serial_write_str("USB: No USB controller found (stub implementation)\n");
    serial_write_str("USB: For keyboard input, use PS/2 emulation or simulated input\n");
    
    g_usb_initialized = true;
    return false;  // No real USB support yet
}

void usb_enumerate_devices(void) {
    if (!g_usb_initialized) return;
    
    // TODO: Implement device enumeration
    // This would:
    // 1. Detect devices on USB ports
    // 2. Reset and configure each device
    // 3. Read descriptors
    // 4. Identify keyboards (class 0x03, protocol 0x01)
    
    serial_write_str("USB: Device enumeration not implemented\n");
}

void usb_poll(void) {
    if (!g_usb_initialized) return;
    
    // TODO: Poll for USB events
    // This would:
    // 1. Check for device attach/detach
    // 2. Poll keyboard interrupt endpoint
    // 3. Process HID reports
}

usb_device_t* usb_get_keyboard(void) {
    // Find first keyboard device
    for (uint8_t i = 0; i < g_usb_hc.num_devices; i++) {
        if (g_usb_hc.devices[i].is_keyboard) {
            return &g_usb_hc.devices[i];
        }
    }
    return NULL;
}

bool usb_keyboard_read(usb_device_t* dev, uint8_t* buffer, uint8_t size) {
    if (!dev || !buffer || size < 8) return false;
    
    // TODO: Implement actual USB transfer
    // This would:
    // 1. Set up interrupt transfer
    // 2. Wait for completion
    // 3. Copy data to buffer
    
    return false;
}

// ===========================================
// SIMULATED USB KEYBOARD (FOR TESTING)
// ===========================================

/*
 * Since full USB implementation is complex, here's how to simulate
 * keyboard input for testing the terminal:
 */

void usb_simulate_key_press(uint8_t scancode, bool shift) {
    usb_hid_keyboard_report_t report = {0};
    
    // Press key
    report.modifiers = shift ? USB_MOD_LSHIFT : 0;
    report.keys[0] = scancode;
    usb_keyboard_process_report(&report);
    
    // Release key
    report.modifiers = 0;
    report.keys[0] = 0;
    usb_keyboard_process_report(&report);
}

void usb_simulate_text(const char* text) {
    // Simulate typing a string
    while (*text) {
        char c = *text++;
        uint8_t scancode = 0;
        bool shift = false;
        
        // Convert character to scancode
        if (c >= 'a' && c <= 'z') {
            scancode = USB_KEY_A + (c - 'a');
        } else if (c >= 'A' && c <= 'Z') {
            scancode = USB_KEY_A + (c - 'A');
            shift = true;
        } else if (c >= '1' && c <= '9') {
            scancode = USB_KEY_1 + (c - '1');
        } else if (c == '0') {
            scancode = USB_KEY_0;
        } else if (c == ' ') {
            scancode = USB_KEY_SPACE;
        } else if (c == '\n') {
            scancode = USB_KEY_ENTER;
        }
        // Add more character mappings as needed
        
        if (scancode) {
            usb_simulate_key_press(scancode, shift);
        }
    }
}

// ===========================================
// IMPLEMENTATION GUIDE
// ===========================================

/*
To implement real USB support, you need:

1. PCI SCANNING (see pci.h/pci.c):
   
   void usb_detect_controllers(void) {
       for (each PCI device) {
           if (class == 0x0C && subclass == 0x03) {
               // This is a USB controller
               uint8_t interface = read_pci_config(dev, 0x09);
               
               if (interface == 0x00) {
                   // UHCI (USB 1.1)
                   usb_init_uhci(dev);
               } else if (interface == 0x20) {
                   // EHCI (USB 2.0)
                   usb_init_ehci(dev);
               } else if (interface == 0x30) {
                   // XHCI (USB 3.0+)
                   usb_init_xhci(dev);
               }
           }
       }
   }

2. UHCI INITIALIZATION (simplest, good starting point):
   
   void usb_init_uhci(pci_device_t* dev) {
       // Get I/O base address
       uint16_t io_base = pci_read_config(dev, 0x20) & 0xFFE0;
       
       // Reset controller
       outw(io_base + USBCMD, 0x0002);  // Global reset
       sleep(50);
       outw(io_base + USBCMD, 0x0000);  // Stop
       
       // Set up frame list
       uint32_t* frame_list = allocate_frame_list();
       outl(io_base + FRBASEADD, (uint32_t)frame_list);
       
       // Start controller
       outw(io_base + USBCMD, 0x0001);  // Run
   }

3. DEVICE ENUMERATION:
   
   void usb_enumerate_port(uint8_t port) {
       // Reset port
       usb_reset_port(port);
       sleep(100);
       
       // Get device descriptor
       usb_device_descriptor_t desc;
       usb_control_transfer(0, USB_REQ_GET_DESCRIPTOR, 
                           (USB_DESC_DEVICE << 8), 0,
                           &desc, sizeof(desc));
       
       // Assign address
       uint8_t addr = usb_allocate_address();
       usb_control_transfer(0, USB_REQ_SET_ADDRESS, addr, 0, NULL, 0);
       
       // Get configuration
       usb_config_descriptor_t config;
       usb_control_transfer(addr, USB_REQ_GET_DESCRIPTOR,
                           (USB_DESC_CONFIGURATION << 8), 0,
                           &config, sizeof(config));
       
       // Check if keyboard
       if (is_keyboard_device(&config)) {
           setup_keyboard_device(addr, &config);
       }
   }

4. KEYBOARD POLLING:
   
   void usb_poll_keyboard(usb_device_t* dev) {
       usb_hid_keyboard_report_t report;
       
       // Read from interrupt endpoint
       if (usb_interrupt_transfer(dev->address, 
                                  dev->keyboard_endpoint,
                                  &report, 8)) {
           // Process the report
           usb_keyboard_process_report(&report);
       }
   }

5. INTERRUPT TRANSFERS (for keyboard polling):
   
   bool usb_interrupt_transfer(uint8_t addr, uint8_t endpoint,
                               void* buffer, uint16_t size) {
       // Create transfer descriptor
       usb_td_t* td = create_interrupt_td(addr, endpoint, buffer, size);
       
       // Add to schedule
       add_to_schedule(td);
       
       // Wait for completion
       while (!td->completed) {
           // Could use interrupts or polling
       }
       
       return td->status == USB_TD_STATUS_SUCCESS;
   }

RECOMMENDED APPROACH:

For now, use the simulated input functions to test your terminal.
When ready to implement real USB:

1. Start with UHCI (simplest)
2. Focus on enumeration first
3. Then implement interrupt transfers for keyboard
4. Test with QEMU's USB keyboard emulation: -usb -device usb-kbd

Example QEMU command:
  qemu-system-x86_64 -drive format=raw,file=kernel.iso \
                     -m 1024 -serial stdio \
                     -usb -device usb-kbd

This will emulate a USB keyboard that you can interact with.
*/