#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PCI_NUM_BARS 6

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
} pci_device_t;


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
void pci_enable_io_busmaster(pci_device_t* dev);
