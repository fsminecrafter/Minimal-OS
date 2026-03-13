#include "usb/usb_stack.h"
#include "usb/uhci.h"
#include "keyboard/usbkeyboard.h"
#include "x86_64/pci.h"
#include "x86_64/port.h"
#include "x86_64/allocator.h"
#include "x86_64/idt.h"
#include "serial.h"
#include "string.h"
#include "time.h"
#include <stdbool.h>

// ===========================================
// EXTERNAL DEPENDENCIES
// ===========================================

// Forward declarations from pci.c
extern uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
extern void pci_write_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

static uint64_t g_last_poll_time[8] = {0};

static uint32_t g_interrupt_attempts = 0;
static uint32_t g_interrupt_successes = 0;
static uint32_t g_interrupt_naks = 0;
static uint32_t g_interrupt_timeouts = 0;
static uint32_t g_interrupt_errors = 0;
static uint32_t g_interrupt_polls;
static uint8_t g_endpoint_toggles[128] = {0};
static uint8_t keyboard_int_buffer[8];
static uint8_t keyboard_toggle = 0;
static bool keyboard_is_low_speed = false;

// ===========================================
// UHCI REGISTERS AND CONSTANTS
// ===========================================

// UHCI Register Offsets (I/O space)
#define UHCI_USBCMD      0x00  // USB Command (16-bit)
#define UHCI_USBSTS      0x02  // USB Status (16-bit)
#define UHCI_USBINTR     0x04  // USB Interrupt Enable (16-bit)
#define UHCI_FRNUM       0x06  // Frame Number (16-bit)
#define UHCI_FRBASEADD   0x08  // Frame List Base Address (32-bit)
#define UHCI_SOFMOD      0x0C  // Start of Frame Modify (8-bit)
#define UHCI_PORTSC1     0x10  // Port 1 Status/Control (16-bit)
#define UHCI_PORTSC2     0x12  // Port 2 Status/Control (16-bit)

// USB Command Register bits
#define UHCI_CMD_RS       (1 << 0)  // Run/Stop
#define UHCI_CMD_HCRESET  (1 << 1)  // Host Controller Reset
#define UHCI_CMD_GRESET   (1 << 2)  // Global Reset
#define UHCI_CMD_EGSM     (1 << 3)  // Enter Global Suspend Mode
#define UHCI_CMD_FGR      (1 << 4)  // Force Global Resume
#define UHCI_CMD_SWDBG    (1 << 5)  // Software Debug
#define UHCI_CMD_CF       (1 << 6)  // Configure Flag
#define UHCI_CMD_MAXP     (1 << 7)  // Max Packet (0=32, 1=64)

// USB Status Register bits
#define UHCI_STS_USBINT   (1 << 0)  // USB Interrupt
#define UHCI_STS_ERROR    (1 << 1)  // USB Error Interrupt
#define UHCI_STS_RESUME   (1 << 2)  // Resume Detect
#define UHCI_STS_HSERR    (1 << 3)  // Host System Error
#define UHCI_STS_HCERR    (1 << 4)  // Host Controller Process Error
#define UHCI_STS_HCH      (1 << 5)  // HC Halted

// Port Status/Control bits
#define UHCI_PORT_CCS     (1 << 0)  // Current Connect Status
#define UHCI_PORT_CSC     (1 << 1)  // Connect Status Change
#define UHCI_PORT_PED     (1 << 2)  // Port Enable/Disable
#define UHCI_PORT_PEDC    (1 << 3)  // Port Enable/Disable Change
#define UHCI_PORT_LSDA    (1 << 8)  // Low Speed Device Attached
#define UHCI_PORT_RD      (1 << 6)  // Resume Detect
#define UHCI_PORT_PR      (1 << 9)  // Port Reset
#define UHCI_PORT_SUSP    (1 << 12) // Suspend

// Transfer Descriptor bits
#define UHCI_TD_ACTIVE    (1 << 23)
#define UHCI_TD_STALLED   (1 << 22)
#define UHCI_TD_DBERR     (1 << 21)
#define UHCI_TD_BABBLE    (1 << 20)
#define UHCI_TD_NAK       (1 << 19)
#define UHCI_TD_CRCTIMEOUT (1 << 18)
#define UHCI_TD_BITSTUFF  (1 << 17)
#define UHCI_TD_IOC       (1 << 24)  // Interrupt on Complete
#define UHCI_TD_IOS       (1 << 25)  // Isochronous Select
#define UHCI_TD_LS        (1 << 26)  // Low Speed Device
#define UHCI_TD_SPD       (1 << 29)  // Short Packet Detect

// USB PID tokens
#define USB_PID_SETUP     0x2D
#define USB_PID_IN        0x69
#define USB_PID_OUT       0xE1

// USB Request Types
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

// USB Descriptor Types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21
#define USB_DESC_REPORT         0x22

#define FRAME_LIST_SIZE 1024

// ===========================================
// UHCI STRUCTURES
// ===========================================

// Transfer Descriptor (16 bytes, 16-byte aligned)
typedef struct {
    uint32_t link_ptr;      // Link to next TD or QH
    uint32_t status;        // Status and control
    uint32_t token;         // PID, device, endpoint, length
    uint32_t buffer_ptr;    // Data buffer physical address
    // Software fields (not used by hardware)
    void* buffer_virt;
    uint32_t reserved[3];
} __attribute__((packed, aligned(16))) uhci_td_t;

// Queue Head (16 bytes, 16-byte aligned)
typedef struct {
    uint32_t head_ptr;      // Queue head link pointer
    uint32_t element_ptr;   // Queue element link pointer
    uint32_t reserved[2];
} __attribute__((packed, aligned(16))) uhci_qh_t;


static uhci_td_t* keyboard_int_td = NULL;
// ===========================================
// GLOBAL USB STATE
// ===========================================

static usb_host_controller_t g_usb_hc = {0};
static bool g_usb_initialized = false;

// UHCI specific state
static uint16_t uhci_io_base = 0;
static uint32_t* frame_list = NULL;
static uhci_qh_t* control_qh = NULL;
static uhci_qh_t* bulk_qh = NULL;
static uhci_qh_t* interrupt_qh = NULL;

// Device tracking
static uint8_t next_address = 1;

// ===========================================
// UHCI REGISTER ACCESS
// ===========================================

static inline uint16_t uhci_read16(uint16_t reg) {
    return port_inw(uhci_io_base + reg);
}

static inline void uhci_write16(uint16_t reg, uint16_t value) {
    port_outw(uhci_io_base + reg, value);
}

static inline uint32_t uhci_read32(uint16_t reg) {
    return port_inl(uhci_io_base + reg);
}

static inline void uhci_write32(uint16_t reg, uint32_t value) {
    port_outl(uhci_io_base + reg, value);
}

static inline uint8_t uhci_read8(uint16_t reg) {
    return port_inb(uhci_io_base + reg);
}

static inline void uhci_write8(uint16_t reg, uint8_t value) {
    port_outb(uhci_io_base + reg, value);
}

// ===========================================
// MEMORY ALLOCATION HELPERS
// ===========================================

static void* uhci_alloc_aligned(size_t size, size_t alignment) {
    // Simple allocator - in production use proper physical memory allocator
    static uint8_t alloc_buffer[0x10000] __attribute__((aligned(4096)));
    static size_t alloc_offset = 0;
    
    // Align offset
    alloc_offset = (alloc_offset + alignment - 1) & ~(alignment - 1);
    
    if (alloc_offset + size > sizeof(alloc_buffer)) {
        serial_write_str("UHCI: Out of memory!\n");
        return NULL;
    }
    
    void* ptr = &alloc_buffer[alloc_offset];
    alloc_offset += size;
    
    // Zero memory
    memset(ptr, 0, size);
    
    return ptr;
}

// ===========================================
// TD/QH MANAGEMENT
// ===========================================

static uhci_td_t* uhci_alloc_td(void) {
    return (uhci_td_t*)uhci_alloc_aligned(sizeof(uhci_td_t), 16);
}

static uhci_qh_t* uhci_alloc_qh(void) {
    return (uhci_qh_t*)uhci_alloc_aligned(sizeof(uhci_qh_t), 16);
}

static void uhci_setup_td(uhci_td_t* td, uint8_t pid, uint8_t dev_addr, 
                          uint8_t endpoint, bool low_speed, void* buffer, 
                          uint16_t length, bool ioc) {
    // Link pointer (terminate)
    td->link_ptr = 1;  // T bit set = terminate
    
    // Status
    td->status = UHCI_TD_ACTIVE | (3 << 27);  // 3 errors allowed
    if (low_speed) {
        td->status |= UHCI_TD_LS;
    }
    if (ioc) {
        td->status |= UHCI_TD_IOC;
    }
    
    // Token: PID | device | endpoint | data toggle | length
    td->token = (pid & 0xFF) | 
                ((dev_addr & 0x7F) << 8) | 
                ((endpoint & 0xF) << 15) |
                ((length & 0x7FF) << 21);
    
    // Buffer
    if (buffer && length > 0) {
        td->buffer_ptr = (uint32_t)(uintptr_t)buffer;
        td->buffer_virt = buffer;
    } else {
        td->buffer_ptr = 0;
        td->buffer_virt = NULL;
    }
}

// ===========================================
// UHCI INITIALIZATION
// ===========================================

bool uhci_init(pci_device_t* dev) {
    serial_write_str("UHCI: Initializing controller\n");
    
    // Get I/O base address from BAR4 (at offset 0x20)
    // Note: BAR4 is at PCI config offset 0x20, not dev->bar[4]!
    uint32_t bar4 = pci_read_config_dword(dev->bus, dev->device, dev->function, 0x20);
    
    serial_write_str("UHCI: BAR4 raw value = 0x");
    serial_write_hex(bar4);
    serial_write_str("\n");
    
    if (!(bar4 & 0x1)) {
        serial_write_str("UHCI: BAR4 is not I/O space!\n");
        serial_write_str("UHCI: Trying BAR0-3...\n");
        
        // Some UHCI controllers use different BARs
        for (int i = 0; i < 5; i++) {
            uint32_t bar = pci_read_config_dword(dev->bus, dev->device, dev->function, 0x10 + i * 4);
            serial_write_str("UHCI: BAR");
            serial_write_dec(i);
            serial_write_str(" = 0x");
            serial_write_hex(bar);
            if (bar & 0x1) {
                serial_write_str(" (I/O)\n");
                bar4 = bar;
                break;
            } else {
                serial_write_str(" (MMIO)\n");
            }
        }
        
        if (!(bar4 & 0x1)) {
            serial_write_str("UHCI: No I/O BAR found!\n");
            return false;
        }
    }
    
    uhci_io_base = bar4 & 0xFFFE;
    serial_write_str("UHCI: I/O Base = 0x");
    serial_write_hex(uhci_io_base);
    serial_write_str("\n");
    
    // Enable bus mastering and I/O space
    uint16_t cmd = pci_config_read_word(dev->bus, dev->device, dev->function, 0x04);
    cmd |= 0x05;  // Bus master + I/O space
    pci_write_config_dword(dev->bus, dev->device, dev->function, 0x04, cmd);
    
    // Reset controller
    serial_write_str("UHCI: Resetting controller\n");
    uhci_write16(UHCI_USBCMD, UHCI_CMD_GRESET);
    sleep(100);
    uhci_write16(UHCI_USBCMD, 0);
    
    uhci_write16(UHCI_USBCMD, UHCI_CMD_HCRESET);
    uint32_t timeout = 1000;
    while ((uhci_read16(UHCI_USBCMD) & UHCI_CMD_HCRESET) && timeout > 0) {
        sleep(1);
        timeout--;
    }
    
    if (timeout == 0) {
        serial_write_str("UHCI: Reset timeout!\n");
        return false;
    }
    
    serial_write_str("UHCI: Reset complete\n");
    
    // Allocate frame list (1024 entries, 4KB aligned)
    frame_list = (uint32_t*)uhci_alloc_aligned(FRAME_LIST_SIZE * sizeof(uint32_t), 4096);
    if (!frame_list) {
        serial_write_str("UHCI: Failed to allocate frame list\n");
        return false;
    }
    
    serial_write_str("UHCI: Frame list at 0x");
    serial_write_hex((uintptr_t)frame_list);
    serial_write_str("\n");
    
    // Initialize frame list (all terminate)
    for (int i = 0; i < FRAME_LIST_SIZE; i++) {
        frame_list[i] = 1;  // Terminate bit
    }
    
    // Create queue heads for different transfer types
    control_qh = uhci_alloc_qh();
    bulk_qh = uhci_alloc_qh();
    interrupt_qh = uhci_alloc_qh();
    
    if (!control_qh || !bulk_qh || !interrupt_qh) {
        serial_write_str("UHCI: Failed to allocate queue heads\n");
        return false;
    }
    
    // Setup queue head chain: interrupt -> control -> bulk
    interrupt_qh->head_ptr = ((uint32_t)(uintptr_t)control_qh) | 0x2;  // QH link
    interrupt_qh->element_ptr = 1;  // Terminate
    
    control_qh->head_ptr = ((uint32_t)(uintptr_t)bulk_qh) | 0x2;
    control_qh->element_ptr = 1;
    
    bulk_qh->head_ptr = 1;  // Terminate
    bulk_qh->element_ptr = 1;
    
    // Point all frame list entries to interrupt QH
    for (int i = 0; i < FRAME_LIST_SIZE; i++) {
        frame_list[i] = ((uint32_t)(uintptr_t)interrupt_qh) | 0x2;  // QH pointer
    }
    
    // Set frame list base address
    uhci_write32(UHCI_FRBASEADD, (uint32_t)(uintptr_t)frame_list);
    
    // Set frame number to 0
    uhci_write16(UHCI_FRNUM, 0);
    
    // Set SOF timing
    uhci_write8(UHCI_SOFMOD, 64);
    
    // Enable interrupts
    uhci_write16(UHCI_USBINTR, 0x000F);  // Enable all interrupts
    
    // Start controller
    uhci_write16(UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    
    serial_write_str("UHCI: Controller started\n");
    
    // Wait for controller to start
    sleep(10);
    
    // Check if halted
    uint16_t status = uhci_read16(UHCI_USBSTS);
    if (status & UHCI_STS_HCH) {
        serial_write_str("UHCI: Controller halted! Status=0x");
        serial_write_hex(status);
        serial_write_str("\n");
        return false;
    }
    
    serial_write_str("UHCI: Controller running\n");
    
    return true;
}

// ===========================================
// PORT MANAGEMENT
// ===========================================

static void uhci_reset_port(uint8_t port) {
    uint16_t port_reg = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    
    serial_write_str("UHCI: Resetting port ");
    serial_write_dec(port);
    serial_write_str("\n");
    
    // Read initial status
    uint16_t val = uhci_read16(port_reg);
    serial_write_str("UHCI: Port status before reset: 0x");
    serial_write_hex(val);
    serial_write_str("\n");
    
    // Disable port first
    val &= ~UHCI_PORT_PED;
    uhci_write16(port_reg, val);
    sleep(20);  // Wait for disable
    
    // Clear any status change bits (write 1 to clear)
    val = uhci_read16(port_reg);
    if (val & UHCI_PORT_CSC) {
        uhci_write16(port_reg, val | UHCI_PORT_CSC);  // Clear CSC
    }
    if (val & UHCI_PORT_PEDC) {
        uhci_write16(port_reg, val | UHCI_PORT_PEDC);  // Clear PEDC
    }
    
    // Start reset - MUST be at least 50ms
    val = uhci_read16(port_reg);
    val |= UHCI_PORT_PR;
    uhci_write16(port_reg, val);
    
    serial_write_str("UHCI: Reset asserted, waiting 50ms\n");
    sleep(50);  // USB spec minimum
    
    // Clear reset
    val = uhci_read16(port_reg);
    val &= ~UHCI_PORT_PR;
    uhci_write16(port_reg, val);
    
    serial_write_str("UHCI: Reset cleared, waiting 10ms recovery\n");
    sleep(10);  // USB spec minimum recovery
    
    // Enable port
    val = uhci_read16(port_reg);
    val |= UHCI_PORT_PED;
    uhci_write16(port_reg, val);
    
    serial_write_str("UHCI: Port enabled, waiting 200ms stabilization\n");
    sleep(200);  // CRITICAL: Give device time to stabilize!
    
    // Check final status
    val = uhci_read16(port_reg);
    serial_write_str("UHCI: Port status after reset: 0x");
    serial_write_hex(val);
    
    if (val & UHCI_PORT_PED) {
        serial_write_str(" [ENABLED]");
    } else {
        serial_write_str(" [DISABLED]");
    }
    
    if (val & UHCI_PORT_CCS) {
        serial_write_str(" [CONNECTED]");
    } else {
        serial_write_str(" [DISCONNECTED]");
    }
    
    if (val & UHCI_PORT_LSDA) {
        serial_write_str(" [LOW-SPEED]");
    } else {
        serial_write_str(" [FULL-SPEED]");
    }
    
    if (val & UHCI_PORT_CSC) {
        serial_write_str(" [STATUS-CHANGED]");
    }
    
    if (val & UHCI_PORT_PEDC) {
        serial_write_str(" [ENABLE-CHANGED]");
    }
    
    serial_write_str("\n");
    
    // Verify port is actually enabled
    if (!(val & UHCI_PORT_PED)) {
        serial_write_str("UHCI: WARNING - Port did not enable!\n");
    }
    
    if (!(val & UHCI_PORT_CCS)) {
        serial_write_str("UHCI: WARNING - Device disconnected!\n");
    }
}

// ===========================================
// CONTROL TRANSFERS
// ===========================================

static void uhci_activate_queue(uhci_qh_t* qh) {
    // Make sure the QH is in the frame list
    // The frame list should already point to interrupt_qh -> control_qh -> bulk_qh
    // but we need to make sure control_qh's element pointer is valid
    
    serial_write_str("UHCI: Activating control queue\n");
    serial_write_str("  QH at 0x");
    serial_write_hex((uintptr_t)qh);
    serial_write_str("\n  element_ptr = 0x");
    serial_write_hex(qh->element_ptr);
    serial_write_str("\n  head_ptr = 0x");
    serial_write_hex(qh->head_ptr);
    serial_write_str("\n");
}

// REPLACE uhci_control_transfer with this version:
bool uhci_control_transfer(uint8_t dev_addr, usb_setup_packet_t* setup, 
                           void* data, uint16_t length) {
    serial_write_str("UHCI: Control transfer to device ");
    serial_write_dec(dev_addr);
    serial_write_str(", length=");
    serial_write_dec(length);
    serial_write_str("\n");
    
    // Allocate TDs
    uhci_td_t* setup_td = uhci_alloc_td();
    uhci_td_t* data_td = NULL;
    uhci_td_t* status_td = uhci_alloc_td();
    
    if (!setup_td || !status_td) {
        serial_write_str("UHCI: Failed to allocate TDs\n");
        return false;
    }
    
    // === SETUP STAGE ===
    setup_td->status = UHCI_TD_ACTIVE | (3 << 27);  // Active, 3 retries
    setup_td->token = (USB_PID_SETUP & 0xFF) |      // PID
                     ((dev_addr & 0x7F) << 8) |      // Device address  
                     ((0 & 0xF) << 15) |             // Endpoint 0
                     (0 << 19) |                     // Data toggle = 0
                     (7 << 21);                      // MaxLength = 7 (for 8 bytes)
    setup_td->buffer_ptr = (uint32_t)(uintptr_t)setup;
    setup_td->buffer_virt = setup;
    setup_td->link_ptr = 0x1;
    
    // === DATA STAGE ===
    if (data && length > 0) {
        data_td = uhci_alloc_td();
        if (!data_td) return false;
        
        uint8_t data_pid = (setup->bmRequestType & 0x80) ? USB_PID_IN : USB_PID_OUT;
        
        data_td->status = UHCI_TD_ACTIVE | (3 << 27);
        if (data_pid == USB_PID_IN) {
            data_td->status |= UHCI_TD_SPD;  // Short packet detect
        }
        data_td->token = (data_pid & 0xFF) |
                        ((dev_addr & 0x7F) << 8) |
                        ((0 & 0xF) << 15) |
                        (1 << 19) |                    // Data toggle = 1
                        (((length - 1) & 0x7FF) << 21);
        data_td->buffer_ptr = (uint32_t)(uintptr_t)data;
        data_td->buffer_virt = data;
        data_td->link_ptr = 0x1;
        
        setup_td->link_ptr = ((uint32_t)(uintptr_t)data_td) & ~0xF;
    }
    
    // === STATUS STAGE ===
    uint8_t status_pid = (data && (setup->bmRequestType & 0x80)) ? USB_PID_OUT : USB_PID_IN;
    
    status_td->status = UHCI_TD_ACTIVE | UHCI_TD_IOC | (3 << 27);
    status_td->token = (status_pid & 0xFF) |
                      ((dev_addr & 0x7F) << 8) |
                      ((0 & 0xF) << 15) |
                      (1 << 19) |                      // Data toggle = 1
                      (0x7FF << 21);                   // 0 bytes
    status_td->buffer_ptr = 0;
    status_td->buffer_virt = NULL;
    status_td->link_ptr = 0x1;
    
    if (data_td) {
        data_td->link_ptr = ((uint32_t)(uintptr_t)status_td) & ~0xF;
    } else {
        setup_td->link_ptr = ((uint32_t)(uintptr_t)status_td) & ~0xF;
    }
    
    // Debug
    serial_write_str("UHCI: TDs created:\n");
    serial_write_str("  Setup: 0x");
    serial_write_hex((uintptr_t)setup_td);
    serial_write_str(" token=0x");
    serial_write_hex(setup_td->token);
    serial_write_str("\n");
    if (data_td) {
        serial_write_str("  Data:  0x");
        serial_write_hex((uintptr_t)data_td);
        serial_write_str(" token=0x");
        serial_write_hex(data_td->token);
        serial_write_str("\n");
    }
    serial_write_str("  Status: 0x");
    serial_write_hex((uintptr_t)status_td);
    serial_write_str(" token=0x");
    serial_write_hex(status_td->token);
    serial_write_str("\n");
    
    // CRITICAL: Add to control queue with proper masking
    uint32_t first_td_addr = ((uint32_t)(uintptr_t)setup_td) & ~0xF;
    control_qh->element_ptr = first_td_addr;
    
    // Verify it was set
    uint32_t verify = control_qh->element_ptr;
    serial_write_str("UHCI: Control QH element_ptr set to 0x");
    serial_write_hex(verify);
    if (verify != first_td_addr) {
        serial_write_str(" ERROR: Mismatch!\n");
        return false;
    }
    serial_write_str(" OK\n");
    
    // IMPORTANT: Ensure controller processes the queue
    // Read/write USBCMD to ensure controller sees the change
    uint16_t usbcmd = uhci_read16(UHCI_USBCMD);
    uhci_write16(UHCI_USBCMD, usbcmd);
    
    // Small delay to let controller pick up the TD
    sleep(1);
    
    // Poll for completion
    uint32_t timeout = 5000;
    uint32_t last_setup_status = 0xFFFFFFFF;
    uint32_t last_status_status = 0xFFFFFFFF;
    
    while (timeout > 0) {
        uint32_t setup_status = setup_td->status;
        uint32_t status_status = status_td->status;
        
        // Log any changes
        if (setup_status != last_setup_status) {
            serial_write_str("UHCI: Setup status: 0x");
            serial_write_hex(setup_status);
            
            // Decode error bits
            if (setup_status & (0x7F << 16)) {
                uint8_t error = (setup_status >> 16) & 0x7F;
                serial_write_str(" errors=0x");
                serial_write_hex(error);
                
                if (setup_status & UHCI_TD_BITSTUFF) serial_write_str(" BITSTUFF");
                if (setup_status & UHCI_TD_CRCTIMEOUT) serial_write_str(" CRC/TIMEOUT");
                if (setup_status & UHCI_TD_NAK) serial_write_str(" NAK");
                if (setup_status & UHCI_TD_BABBLE) serial_write_str(" BABBLE");
                if (setup_status & UHCI_TD_DBERR) serial_write_str(" DBERR");
                if (setup_status & UHCI_TD_STALLED) serial_write_str(" STALL");
            }
            serial_write_str("\n");
            last_setup_status = setup_status;
        }
        
        if (status_status != last_status_status) {
            serial_write_str("UHCI: Status status: 0x");
            serial_write_hex(status_status);
            serial_write_str("\n");
            last_status_status = status_status;
        }
        
        // Check if completed
        if (!(status_status & UHCI_TD_ACTIVE)) {
            serial_write_str("UHCI: Transfer completed!\n");
            break;
        }
        
        // Check if setup stage failed
        if (!(setup_status & UHCI_TD_ACTIVE)) {
            // Setup completed but status still active - check for errors
            if (setup_status & (UHCI_TD_STALLED | UHCI_TD_DBERR | UHCI_TD_BABBLE | 
                               UHCI_TD_CRCTIMEOUT | UHCI_TD_BITSTUFF)) {
                serial_write_str("UHCI: Setup stage failed\n");
                break;
            }
        }
        
        sleep(1);
        timeout--;
        
        // Every 500ms, print diagnostics
        if (timeout % 500 == 0) {
            uint16_t frnum = uhci_read16(UHCI_FRNUM);
            uint16_t usbsts = uhci_read16(UHCI_USBSTS);
            serial_write_str("UHCI: FRNUM=");
            serial_write_dec(frnum);
            serial_write_str(" USBSTS=0x");
            serial_write_hex(usbsts);
            serial_write_str(" QH_element=0x");
            serial_write_hex(control_qh->element_ptr);
            serial_write_str("\n");
            
            // Clear any status bits
            if (usbsts) {
                uhci_write16(UHCI_USBSTS, usbsts);
            }
        }
    }
    
    // Clear queue
    control_qh->element_ptr = 0x1;
    
    if (timeout == 0) {
        serial_write_str("UHCI: Control transfer TIMEOUT\n");
        serial_write_str("  Final statuses:\n");
        serial_write_str("    Setup:  0x");
        serial_write_hex(setup_td->status);
        serial_write_str("\n");
        if (data_td) {
            serial_write_str("    Data:   0x");
            serial_write_hex(data_td->status);
            serial_write_str("\n");
        }
        serial_write_str("    Status: 0x");
        serial_write_hex(status_td->status);
        serial_write_str("\n");
        return false;
    }
    
    // Check for errors
    if (status_td->status & (UHCI_TD_STALLED | UHCI_TD_DBERR | UHCI_TD_BABBLE | 
                             UHCI_TD_CRCTIMEOUT | UHCI_TD_BITSTUFF)) {
        serial_write_str("UHCI: Control transfer ERROR\n");
        return false;
    }
    
    serial_write_str("UHCI: Control transfer SUCCESS\n");
    return true;
}

// ===========================================
// USB INITIALIZATION (PUBLIC API)
// ===========================================

bool usb_init(void) {
    serial_write_str("USB: Initializing USB subsystem...\n");
    
    g_usb_hc.type = USB_HC_TYPE_NONE;
    g_usb_hc.initialized = false;
    g_usb_hc.num_devices = 0;
    
    // Scan PCI for USB controllers
    serial_write_str("USB: Scanning PCI for USB controllers...\n");
    
    extern pci_device_t pci_devices[];
    extern int pci_device_count;
    
    bool found_controller = false;
    
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* dev = &pci_devices[i];
        
        // USB controller: class 0x0C, subclass 0x03
        if (dev->class_code == 0x0C && dev->subclass == 0x03) {
            serial_write_str("USB: Found USB controller at ");
            serial_write_dec(dev->bus);
            serial_write_str(":");
            serial_write_dec(dev->device);
            serial_write_str(":");
            serial_write_dec(dev->function);
            serial_write_str(" - VID:PID ");
            serial_write_hex(dev->vendor_id);
            serial_write_str(":");
            serial_write_hex(dev->device_id);
            serial_write_str(" prog_if=0x");
            serial_write_hex(dev->prog_if);
            serial_write_str("\n");
            
            // Skip if we already initialized a controller
            if (found_controller) {
                serial_write_str("USB: Skipping (already have a controller)\n");
                continue;
            }
            
            if (dev->prog_if == 0x00) {
                // UHCI
                serial_write_str("USB: Type: UHCI (USB 1.1)\n");
                if (uhci_init(dev)) {
                    g_usb_hc.type = USB_HC_TYPE_UHCI;
                    g_usb_hc.io_base = uhci_io_base;
                    g_usb_hc.initialized = true;
                    g_usb_initialized = true;
                    found_controller = true;
                    
                    // Enumerate devices on ports
                    usb_enumerate_devices();
                    usb_device_t* kbd = usb_get_keyboard();
                    if (kbd) {
                        uhci_keyboard_interrupt_init(kbd);
                    }
                    
                    return true;
                }
            } else if (dev->prog_if == 0x10) {
                serial_write_str("USB: Type: OHCI (USB 1.1) - not yet supported\n");
            } else if (dev->prog_if == 0x20) {
                serial_write_str("USB: Type: EHCI (USB 2.0) - not yet supported\n");
            } else if (dev->prog_if == 0x30) {
                serial_write_str("USB: Type: XHCI (USB 3.0+) - not yet supported\n");
            } else {
                serial_write_str("USB: Unknown type\n");
            }
        }
    }
    
    serial_write_str("USB: No supported USB controller found\n");
    return false;
}

// ===========================================
// INTERRUPT TRANSFERS
// ===========================================
/*
bool uhci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint, void* buffer, uint16_t length, bool low_speed) {
    g_interrupt_attempts++;
    
    // Calculate toggle key and get current toggle
    uint8_t toggle_key = (dev_addr << 4) | (endpoint & 0xF);
    uint8_t toggle = g_endpoint_toggles[toggle_key];
    
    // Debug first few attempts
    if (g_interrupt_attempts <= 5) {
        serial_write_str("USB: Interrupt #");
        serial_write_dec(g_interrupt_attempts);
        serial_write_str(" dev=");
        serial_write_dec(dev_addr);
        serial_write_str(" ep=");
        serial_write_dec(endpoint);
        serial_write_str(" toggle=");
        serial_write_dec(toggle);
        serial_write_str("\n");
    }
    
    // Allocate TD
    uhci_td_t* td = uhci_alloc_td();
    if (!td) {
        serial_write_str("USB: Failed to allocate TD!\n");
        return false;
    }
    
    // Setup TD with correct toggle
    td->status = UHCI_TD_ACTIVE | (3 << 27);
    if (low_speed) {
        td->status |= UHCI_TD_LS;
    }
    td->status |= UHCI_TD_SPD;
    
    td->token = (USB_PID_IN & 0xFF) |
               ((dev_addr & 0x7F) << 8) |
               ((endpoint & 0xF) << 15) |
               (toggle << 19) |
               (((length - 1) & 0x7FF) << 21);
    
    td->buffer_ptr = (uint32_t)(uintptr_t)buffer;
    td->buffer_virt = buffer;
    td->link_ptr = 0x1;
    
    // Add to interrupt queue
    interrupt_qh->element_ptr = ((uint32_t)(uintptr_t)td) & ~0xF;
    
    // Wait for completion
    uint32_t timeout = 20;
    while (timeout > 0) {
        if (!(td->status & UHCI_TD_ACTIVE)) {
            break;
        }
        sleep(1);
        timeout--;
    }
    
    // Clear queue
    interrupt_qh->element_ptr = 0x1;
    
    // Check result
    uint32_t status = td->status;
    
    if (timeout == 0) {
        g_interrupt_timeouts++;
        if (g_interrupt_timeouts <= 5) {
            serial_write_str("USB: Interrupt timeout #");
            serial_write_dec(g_interrupt_timeouts);
            serial_write_str(" status=0x");
            serial_write_hex(status);
            serial_write_str("\n");
        }
        return false;
    }
    
    // Check for NAK (normal - no data)
    if (status & UHCI_TD_NAK) {
        g_interrupt_naks++;
        return false;
    }
    
    // Check for errors
    if (status & (UHCI_TD_STALLED | UHCI_TD_DBERR | UHCI_TD_BABBLE | 
                  UHCI_TD_CRCTIMEOUT | UHCI_TD_BITSTUFF)) {
        g_interrupt_errors++;
        if (g_interrupt_errors <= 10) {
            serial_write_str("USB: Interrupt ERROR #");
            serial_write_dec(g_interrupt_errors);
            serial_write_str(" status=0x");
            serial_write_hex(status);
            if (status & UHCI_TD_STALLED) serial_write_str(" STALL");
            if (status & UHCI_TD_DBERR) serial_write_str(" DBERR");
            if (status & UHCI_TD_BABBLE) serial_write_str(" BABBLE");
            if (status & UHCI_TD_CRCTIMEOUT) serial_write_str(" CRC/TIMEOUT");
            if (status & UHCI_TD_BITSTUFF) serial_write_str(" BITSTUFF");
            serial_write_str("\n");
        }
        return false;
    }
    
    // Success!
    g_interrupt_successes++;
    g_endpoint_toggles[toggle_key] = toggle ^ 1;  // Flip toggle
    
    return true;
}
*/
// ===========================================
// KEYBOARD POLLING
// ===========================================

void uhci_keyboard_interrupt_init(usb_device_t* dev) {
    serial_write_str("UHCI: Setting up persistent keyboard interrupt TD\n");
    
    keyboard_int_td = uhci_alloc_td();
    if (!keyboard_int_td) {
        serial_write_str("UHCI: CRITICAL - Failed to allocate keyboard TD!\n");
        return;
    }

    // Clear report buffer
    memset(keyboard_int_buffer, 0, sizeof(keyboard_int_buffer));

    uint16_t max_packet = dev->keyboard_max_packet_size;
    if (max_packet == 0 || max_packet > 8) {
        max_packet = 8;
    }

    keyboard_is_low_speed = dev->low_speed;

    uint32_t td_phys = ((uint32_t)(uintptr_t)keyboard_int_td) & ~0xF;

    // ============================
    // STATUS
    // ============================
    keyboard_int_td->status =
        UHCI_TD_ACTIVE |
        UHCI_TD_IOC |
        UHCI_TD_SPD |
        (3 << 27) |
        0x7FF;                // reset actual length

    if (keyboard_is_low_speed) {
        keyboard_int_td->status |= UHCI_TD_LS;
    }

    // ============================
    // TOKEN
    // ============================
    keyboard_int_td->token =
        (USB_PID_IN & 0xFF) |
        ((dev->address & 0x7F) << 8) |
        ((dev->keyboard_endpoint & 0xF) << 15) |
        (0 << 20) |                       // DATA0 toggle
        (((max_packet - 1) & 0x7FF) << 21);

    // ============================
    // BUFFER
    // ============================
    keyboard_int_td->buffer_ptr = (uint32_t)(uintptr_t)keyboard_int_buffer;
    keyboard_int_td->buffer_virt = keyboard_int_buffer;

    // ============================
    // CRITICAL FIX: TD LOOP
    // ============================
    keyboard_int_td->link_ptr = td_phys;   // loop to itself

    // Attach TD to interrupt queue
    interrupt_qh->element_ptr = td_phys;

    keyboard_toggle = 0;

    serial_write_str("UHCI: Keyboard TD installed\n");
    serial_write_str("  Device addr: ");
    serial_write_dec(dev->address);
    serial_write_str("\n  Endpoint: ");
    serial_write_dec(dev->keyboard_endpoint);
    serial_write_str("\n  Max packet: ");
    serial_write_dec(max_packet);
    serial_write_str("\n  TD phys: 0x");
    serial_write_hex(td_phys);
    serial_write_str("\n");
}

// Reactivate keyboard TD for next transfer
void uhci_keyboard_interrupt_reactivate(void)
{
    if (!keyboard_int_td) return;

    memset(keyboard_int_buffer, 0, 8);

    uint32_t token = keyboard_int_td->token;

    token &= ~(1 << 20);
    token |= (keyboard_toggle << 20);

    keyboard_int_td->token = token;

    uint32_t status =
        UHCI_TD_ACTIVE |
        UHCI_TD_IOC |
        UHCI_TD_SPD |
        (3 << 27) |
        0x7FF;              // <-- REQUIRED (reset actual length)

    if (keyboard_is_low_speed)
        status |= UHCI_TD_LS;

    keyboard_int_td->status = status;
}

bool uhci_keyboard_interrupt_poll(uint8_t* buffer, uint16_t length) {
    static uint32_t poll_count = 0;
    poll_count++;
    
    if (!keyboard_int_td) {
        return false;
    }
    
    g_interrupt_polls++;
    uint32_t status = keyboard_int_td->status;
    
    // Debug EVERY poll for first 20, then every 100
    if (poll_count <= 20 || (poll_count % 100) == 0) {
        serial_write_str("UHCI: Poll #");
        serial_write_dec(poll_count);
        serial_write_str(" TD status=0x");
        serial_write_hex(status);
        
        if (status & UHCI_TD_ACTIVE) {
            serial_write_str(" [ACTIVE - waiting]");
        } else {
            serial_write_str(" [COMPLETE");
            if (status & UHCI_TD_NAK) serial_write_str(" NAK");
            if (status & UHCI_TD_STALLED) serial_write_str(" STALL");
            if (status & UHCI_TD_DBERR) serial_write_str(" DBERR");
            if (status & UHCI_TD_BABBLE) serial_write_str(" BABBLE");
            if (status & UHCI_TD_CRCTIMEOUT) serial_write_str(" CRC");
            if (status & UHCI_TD_BITSTUFF) serial_write_str(" BITSTUFF");
            serial_write_str("]");
        }
        serial_write_str("\n");
    }
    
    // Check if TD is still active
    if (status & UHCI_TD_ACTIVE) {
        // Still active - no data yet
        return false;
    }
    
    // TD completed! Check for NAK
    if (status & UHCI_TD_NAK) {
        g_interrupt_naks++;
        uhci_keyboard_interrupt_reactivate();
        return false;
    }
    
    // Check for other errors
    if (status & (UHCI_TD_STALLED | UHCI_TD_DBERR | UHCI_TD_BABBLE | 
                  UHCI_TD_CRCTIMEOUT | UHCI_TD_BITSTUFF)) {
        g_interrupt_errors++;
        if (g_interrupt_errors <= 10) {
            serial_write_str("UHCI: Keyboard error #");
            serial_write_dec(g_interrupt_errors);
            serial_write_str(": 0x");
            serial_write_hex(status);
            serial_write_str("\n");
        }
        uhci_keyboard_interrupt_reactivate();
        return false;
    }
    
    // Success! Copy data
    g_interrupt_successes++;
    for (int i = 0; i < length && i < 8; i++) {
        buffer[i] = keyboard_int_buffer[i];
    }
    
    // Flip toggle and reactivate
    keyboard_toggle ^= 1;
    uhci_keyboard_interrupt_reactivate();
    
    return true;
}

 
void usb_poll_keyboard(usb_device_t* dev) {
    static uint8_t report_buffer[8];
    
    // Poll the persistent TD
    if (uhci_keyboard_interrupt_poll(report_buffer, 8)) {
        g_interrupt_successes++;
        
        // Debug: print report
        serial_write_str("USB: HID #");
        serial_write_dec(g_interrupt_successes);
        serial_write_str(": ");
        for (int i = 0; i < 8; i++) {
            if (report_buffer[i] < 0x10) serial_write_str("0");
            serial_write_hex(report_buffer[i]);
            serial_write_str(" ");
        }
        serial_write_str("\n");
        
        // ALWAYS process the report - don't filter!
        // The keyboard driver handles key transitions internally
        usb_hid_keyboard_report_t* report = (usb_hid_keyboard_report_t*)report_buffer;
        usb_keyboard_process_report(report);
    }
    
    // Print stats every 500 polls
    if ((g_interrupt_polls % 500) == 0 && g_interrupt_polls > 0) {
        serial_write_str("USB: Stats - polls=");
        serial_write_dec(g_interrupt_polls);
        serial_write_str(" success=");
        serial_write_dec(g_interrupt_successes);
        serial_write_str(" NAKs=");
        serial_write_dec(g_interrupt_naks);
        serial_write_str("\n");
    }
}
 
// Call this ONCE after keyboard enumeration
void usb_init_keyboard_polling(usb_device_t* dev) {
    uhci_keyboard_interrupt_init(dev);
}

// ===========================================
// DEVICE ENUMERATION
// ===========================================

void usb_enumerate_devices(void) {
    if (!g_usb_initialized) return;
    
    serial_write_str("USB: Enumerating devices...\n");
    
    // Check both ports
    for (uint8_t port = 0; port < 2; port++) {
        uint16_t port_reg = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t port_status = uhci_read16(port_reg);
        
        serial_write_str("USB: Port ");
        serial_write_dec(port);
        serial_write_str(" status: 0x");
        serial_write_hex(port_status);
        serial_write_str("\n");
        
        // Check if device connected
        if (port_status & UHCI_PORT_CCS) {
            serial_write_str("USB: Device connected on port ");
            serial_write_dec(port);
            serial_write_str("\n");
            
            // Reset port
            uhci_reset_port(port);
            
            // Check if low speed
            port_status = uhci_read16(port_reg);
            bool low_speed = (port_status & UHCI_PORT_LSDA) != 0;
            
            serial_write_str("USB: Device is ");
            serial_write_str(low_speed ? "LOW" : "FULL");
            serial_write_str(" speed\n");
            
            // Enumerate device
            if (usb_enumerate_device(port, low_speed)) {
                serial_write_str("USB: Device enumeration successful\n");
            } else {
                serial_write_str("USB: Device enumeration failed\n");
            }
        }
    }
    
    serial_write_str("USB: Enumeration complete, found ");
    serial_write_dec(g_usb_hc.num_devices);
    serial_write_str(" devices\n");
}

bool usb_enumerate_device(uint8_t port, bool low_speed) {
    serial_write_str("USB: Enumerating device on port ");
    serial_write_dec(port);
    serial_write_str("\n");
    
    // Wait for device to stabilize
    sleep(100);
    
    // Allocate device structure
    if (g_usb_hc.num_devices >= 8) {
        serial_write_str("USB: Too many devices!\n");
        return false;
    }
    
    usb_device_t* dev = &g_usb_hc.devices[g_usb_hc.num_devices];
    memset(dev, 0, sizeof(usb_device_t));
    dev->port = port;
    dev->address = 0;  // Default address
    dev->state = USB_DEVICE_STATE_DEFAULT;
    
    // Step 1: Get first 8 bytes of device descriptor (to get max packet size)
    serial_write_str("USB: Getting device descriptor (8 bytes)\n");
    
    usb_device_descriptor_t desc;
    usb_setup_packet_t setup = {
        .bmRequestType = 0x80,  // Device to host
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (USB_DESC_DEVICE << 8) | 0,
        .wIndex = 0,
        .wLength = 8
    };
    
    if (!uhci_control_transfer(0, &setup, &desc, 8)) {
        serial_write_str("USB: Failed to get device descriptor\n");
        return false;
    }
    
    serial_write_str("USB: Max packet size: ");
    serial_write_dec(desc.bMaxPacketSize0);
    serial_write_str("\n");
    
    // Step 2: Set address
    uint8_t new_addr = next_address++;
    serial_write_str("USB: Setting address to ");
    serial_write_dec(new_addr);
    serial_write_str("\n");
    
    setup.bmRequestType = 0x00;  // Host to device
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = new_addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    
    if (!uhci_control_transfer(0, &setup, NULL, 0)) {
        serial_write_str("USB: Failed to set address\n");
        return false;
    }
    
    sleep(10);  // Give device time to process
    
    dev->address = new_addr;
    dev->state = USB_DEVICE_STATE_ADDRESS;
    
    // Step 3: Get full device descriptor
    serial_write_str("USB: Getting full device descriptor\n");
    
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = sizeof(usb_device_descriptor_t);
    
    if (!uhci_control_transfer(new_addr, &setup, &desc, sizeof(desc))) {
        serial_write_str("USB: Failed to get full device descriptor\n");
        return false;
    }
    
    // Store device info
    dev->vendor_id = desc.idVendor;
    dev->product_id = desc.idProduct;
    dev->class_code = desc.bDeviceClass;
    dev->subclass = desc.bDeviceSubClass;
    dev->protocol = desc.bDeviceProtocol;
    
    serial_write_str("USB: Device: VID=0x");
    serial_write_hex(dev->vendor_id);
    serial_write_str(" PID=0x");
    serial_write_hex(dev->product_id);
    serial_write_str(" Class=0x");
    serial_write_hex(dev->class_code);
    serial_write_str("\n");
    
    // Step 4: Get configuration descriptor
    serial_write_str("USB: Getting configuration descriptor\n");
    
    usb_config_descriptor_t config;
    setup.wValue = (USB_DESC_CONFIGURATION << 8) | 0;
    setup.wLength = sizeof(config);
    
    if (!uhci_control_transfer(new_addr, &setup, &config, sizeof(config))) {
        serial_write_str("USB: Failed to get configuration descriptor\n");
        return false;
    }
    
    serial_write_str("USB: Configuration: interfaces=");
    serial_write_dec(config.bNumInterfaces);
    serial_write_str(" total_length=");
    serial_write_dec(config.wTotalLength);
    serial_write_str("\n");
    
    // Allocate buffer for full configuration
    uint8_t config_buffer[256];
    if (config.wTotalLength > sizeof(config_buffer)) {
        serial_write_str("USB: Configuration too large!\n");
        return false;
    }
    
    // Get full configuration
    setup.wLength = config.wTotalLength;
    if (!uhci_control_transfer(new_addr, &setup, config_buffer, config.wTotalLength)) {
        serial_write_str("USB: Failed to get full configuration\n");
        return false;
    }
    
    // Parse configuration for keyboard
    bool is_keyboard = usb_parse_configuration(dev, config_buffer, config.wTotalLength);
    
    if (is_keyboard) {
        serial_write_str("USB: Keyboard detected!\n");
        dev->is_keyboard = true;
        
        // Set configuration
        serial_write_str("USB: Setting configuration\n");
        setup.bmRequestType = 0x00;
        setup.bRequest = USB_REQ_SET_CONFIGURATION;
        setup.wValue = config.bConfigurationValue;
        setup.wIndex = 0;
        setup.wLength = 0;
        
        if (!uhci_control_transfer(new_addr, &setup, NULL, 0)) {
            serial_write_str("USB: Failed to set configuration\n");
            return false;
        }
        
        dev->config_value = config.bConfigurationValue;
        dev->state = USB_DEVICE_STATE_CONFIGURED;
        
        // Initialize keyboard
        usb_keyboard_init_device(dev);
    }
    
    g_usb_hc.num_devices++;
    return true;
}

// ===========================================
// CONFIGURATION PARSING
// ===========================================

bool usb_parse_configuration(usb_device_t* dev, uint8_t* config_data, uint16_t length) {
    uint16_t offset = 0;
    bool found_keyboard = false;
    
    while (offset < length) {
        uint8_t desc_len = config_data[offset];
        uint8_t desc_type = config_data[offset + 1];
        
        if (desc_len == 0) break;
        
        if (desc_type == USB_DESC_INTERFACE) {
            usb_interface_descriptor_t* iface = (usb_interface_descriptor_t*)&config_data[offset];
            
            serial_write_str("USB:   Interface: class=0x");
            serial_write_hex(iface->bInterfaceClass);
            serial_write_str(" subclass=0x");
            serial_write_hex(iface->bInterfaceSubClass);
            serial_write_str(" protocol=0x");
            serial_write_hex(iface->bInterfaceProtocol);
            serial_write_str("\n");
            
            // HID class (0x03), Boot interface subclass (0x01), Keyboard protocol (0x01)
            if (iface->bInterfaceClass == 0x03 &&
                iface->bInterfaceSubClass == 0x01 &&
                iface->bInterfaceProtocol == 0x01) {
                serial_write_str("USB:   -> This is a HID Boot Keyboard!\n");
                found_keyboard = true;
            }
        }
        else if (desc_type == USB_DESC_ENDPOINT) {
            usb_endpoint_descriptor_t* ep = (usb_endpoint_descriptor_t*)&config_data[offset];
            
            serial_write_str("USB:   Endpoint: addr=0x");
            serial_write_hex(ep->bEndpointAddress);
            serial_write_str(" attr=0x");
            serial_write_hex(ep->bmAttributes);
            serial_write_str(" maxpkt=");
            serial_write_dec(ep->wMaxPacketSize);
            serial_write_str(" interval=");
            serial_write_dec(ep->bInterval);
            serial_write_str("\n");
            
            // Save keyboard interrupt endpoint (IN, interrupt)
            if (found_keyboard && 
                (ep->bEndpointAddress & 0x80) &&  // IN endpoint
                (ep->bmAttributes & 0x03) == 0x03) {  // Interrupt transfer
                dev->keyboard_endpoint = ep->bEndpointAddress & 0x0F;
                dev->keyboard_max_packet_size = ep->wMaxPacketSize;
                dev->keyboard_interval = ep->bInterval;
                
                serial_write_str("USB:   -> Keyboard interrupt endpoint found!\n");
            }
        }
        
        offset += desc_len;
    }
    
    return found_keyboard;
}

// ===========================================
// KEYBOARD INITIALIZATION
// ===========================================

bool usb_keyboard_init_device(usb_device_t* dev) {
    serial_write_str("USB: Initializing keyboard device\n");
    
    // Set boot protocol
    usb_setup_packet_t setup = {
        .bmRequestType = 0x21,  // Class, Interface
        .bRequest = 0x0B,       // SET_PROTOCOL
        .wValue = 0,            // Boot protocol
        .wIndex = 0,            // Interface 0
        .wLength = 0
    };
    
    if (!uhci_control_transfer(dev->address, &setup, NULL, 0)) {
        serial_write_str("USB: Failed to set boot protocol\n");
        return false;
    }
    
    serial_write_str("USB: Keyboard initialized successfully!\n");
    serial_write_str("USB:   Endpoint: ");
    serial_write_dec(dev->keyboard_endpoint);
    serial_write_str("\n");
    serial_write_str("USB:   Max packet: ");
    serial_write_dec(dev->keyboard_max_packet_size);
    serial_write_str("\n");
    serial_write_str("USB:   Interval: ");
    serial_write_dec(dev->keyboard_interval);
    serial_write_str("ms\n");
    
    return true;
}

// ===========================================
// USB POLLING
// ===========================================
void usb_poll(void) {
    static bool first_poll = true;
    
    if (!g_usb_initialized) return;
    
    if (first_poll) {
        serial_write_str("USB: Starting keyboard polling...\n");
        first_poll = false;
    }
    
    uint64_t now = time_get_uptime_ms();
    
    for (uint8_t i = 0; i < g_usb_hc.num_devices; i++) {
        usb_device_t* dev = &g_usb_hc.devices[i];
        
        if (!dev->is_keyboard || dev->state != USB_DEVICE_STATE_CONFIGURED) {
            continue;
        }
        
        // Poll at keyboard's interval (10ms)
        uint64_t elapsed = now - dev->last_poll_time;
        if (elapsed < dev->keyboard_interval) {
            continue;
        }
        
        dev->last_poll_time = now;
        usb_poll_keyboard(dev);
    }
}

usb_device_t* usb_get_keyboard(void) {
    for (uint8_t i = 0; i < g_usb_hc.num_devices; i++) {
        if (g_usb_hc.devices[i].is_keyboard) {
            return &g_usb_hc.devices[i];
        }
    }
    return NULL;
}

// Continue with interrupt transfers in next file...
