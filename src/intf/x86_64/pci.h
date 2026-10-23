#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/gpu_hw.h"

#define PCI_NUM_BARS 6
#define PCI_MAX_DEVICES 64

typedef enum {
    PCI_BAR_UNUSED = 0,
    PCI_BAR_IO = 1,
    PCI_BAR_MEM = 2,
} pci_bar_type_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint32_t bar[PCI_NUM_BARS];              // Store BAR addresses
    pci_bar_type_t bar_type[PCI_NUM_BARS];  // IO or MEM type per BAR
    // Full BAR information. A 64-bit memory BAR occupies two slots; the
    // first holds the whole address and size, the second is UNUSED.
    uint64_t bar_address[PCI_NUM_BARS];
    uint64_t bar_length[PCI_NUM_BARS];
    bool bar_is_64[PCI_NUM_BARS];
    bool bar_prefetchable[PCI_NUM_BARS];
} pci_device_t;

extern pci_device_t pci_devices[PCI_MAX_DEVICES];
extern int pci_device_count;


static inline void pci_write_address(uint32_t address);
static inline uint32_t pci_read_data();
uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
bool pci_device_present(uint8_t bus, uint8_t device, uint8_t function);
void pci_enumerate_bus(uint8_t bus);
void pci_enumerate_all();
void print_pci_devices(); //Prints in the serial COM1 output
uint32_t pci_read_data(void);
void test_gpu();
void initializeGraphicsDevice();

int pci_get_device_count(void);
pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);
// Also wakes the device to D0 (see pci_set_power_d0).
void pci_enable_io_busmaster(pci_device_t* dev);
uint8_t pci_find_capability(pci_device_t* dev, uint8_t id);
void pci_set_power_d0(pci_device_t* dev);

/* Virtual buses: devices that are not reached through the 0xCF8/0xCFC
 * ports (for example PCI devices passed through to a Hyper-V VM, whose
 * config space goes through VMBus). Buses from PCI_VBUS_FIRST up are
 * routed to the accessors registered here; offsets are dword aligned. */
#define PCI_VBUS_FIRST 0xF0
typedef uint32_t (*pci_vbus_read_t)(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
typedef void (*pci_vbus_write_t)(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset,
                                 uint32_t value);
void pci_set_vbus_accessors(pci_vbus_read_t read, pci_vbus_write_t write);
bool pci_vbus_read(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t* value);
bool pci_vbus_write(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value);
// Appends a device found outside the normal scan and lets class drivers
// attach. NULL if the table is full.
pci_device_t* pci_add_device(const pci_device_t* dev);

// Index of the memory BAR that contains [phys, phys+len), or -1.
int pci_bar_containing(const pci_device_t* dev, uint64_t phys, uint64_t len);

// Returns the gpu_hw_driver_t vtable for registration with
// gpu_manager_register_driver(). Prefer this + gpu_manager_init()
// over calling initializeGraphicsDevice() directly - see the comment
// above gpu_bochs_vbe_init() in pci.c for why.
const gpu_hw_driver_t* gpu_bochs_vbe_get_driver(void);
