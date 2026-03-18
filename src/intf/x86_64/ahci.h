#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/pci.h"

/*
 * AHCI (Advanced Host Controller Interface) Driver
 * For SATA disk access
 */

// ===========================================
// AHCI CONSTANTS
// ===========================================

#define AHCI_CLASS_CODE     0x01    // Mass Storage Controller
#define AHCI_SUBCLASS       0x06    // SATA Controller
#define AHCI_PROG_IF        0x01    // AHCI 1.0

#define SATA_SIG_ATA        0x00000101  // SATA drive
#define SATA_SIG_ATAPI      0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB       0xC33C0101  // Enclosure management bridge
#define SATA_SIG_PM         0x96690101  // Port multiplier

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

#define ATA_DEV_BUSY        0x80
#define ATA_DEV_DRQ         0x08

#define ATA_CMD_READ_DMA_EX     0x25
#define ATA_CMD_WRITE_DMA_EX    0x35
#define ATA_CMD_IDENTIFY        0xEC

// ===========================================
// AHCI STRUCTURES
// ===========================================

typedef volatile struct {
    uint32_t clb;       // Command list base address (1KB aligned)
    uint32_t clbu;      // Command list base address upper 32 bits
    uint32_t fb;        // FIS base address (256B aligned)
    uint32_t fbu;       // FIS base address upper 32 bits
    uint32_t is;        // Interrupt status
    uint32_t ie;        // Interrupt enable
    uint32_t cmd;       // Command and status
    uint32_t rsv0;      // Reserved
    uint32_t tfd;       // Task file data
    uint32_t sig;       // Signature
    uint32_t ssts;      // SATA status (SCR0:SStatus)
    uint32_t sctl;      // SATA control (SCR2:SControl)
    uint32_t serr;      // SATA error (SCR1:SError)
    uint32_t sact;      // SATA active (SCR3:SActive)
    uint32_t ci;        // Command issue
    uint32_t sntf;      // SATA notification (SCR4:SNotification)
    uint32_t fbs;       // FIS-based switch control
    uint32_t rsv1[11];  // Reserved
    uint32_t vendor[4]; // Vendor specific
} hba_port_t;

typedef volatile struct {
    uint32_t cap;       // Host capability
    uint32_t ghc;       // Global host control
    uint32_t is;        // Interrupt status
    uint32_t pi;        // Port implemented
    uint32_t vs;        // Version
    uint32_t ccc_ctl;   // Command completion coalescing control
    uint32_t ccc_pts;   // Command completion coalescing ports
    uint32_t em_loc;    // Enclosure management location
    uint32_t em_ctl;    // Enclosure management control
    uint32_t cap2;      // Host capabilities extended
    uint32_t bohc;      // BIOS/OS handoff control and status
    uint8_t rsv[0xA0-0x2C];
    uint8_t vendor[0x100-0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t rsv0:3;
    uint8_t c:1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

typedef struct {
    uint8_t cfl:5;      // Command FIS length in DWORDS
    uint8_t a:1;        // ATAPI
    uint8_t w:1;        // Write
    uint8_t p:1;        // Prefetchable
    uint8_t r:1;        // Reset
    uint8_t b:1;        // BIST
    uint8_t c:1;        // Clear busy upon R_OK
    uint8_t rsv0:1;
    uint8_t pmp:4;      // Port multiplier port
    uint16_t prdtl;     // PRDT length
    volatile uint32_t prdbc;  // PRD byte count
    uint32_t ctba;      // Command table base address
    uint32_t ctbau;     // Command table base address upper 32 bits
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint32_t dba;       // Data base address
    uint32_t dbau;      // Data base address upper 32 bits
    uint32_t rsv0;
    uint32_t dbc:22;    // Byte count
    uint32_t rsv1:9;
    uint32_t i:1;       // Interrupt on completion
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];           // Command FIS (64 bytes)
    uint8_t acmd[16];           // ATAPI command
    uint8_t rsv[48];
    hba_prdt_entry_t prdt_entry[1];
} __attribute__((packed)) hba_cmd_tbl_t;

// ===========================================
// AHCI DRIVE INFO
// ===========================================

typedef struct {
    bool present;
    uint8_t port_num;
    hba_port_t* port;
    uint32_t signature;
    uint64_t sectors;       // Total sectors
    uint32_t sector_size;   // Bytes per sector (usually 512)
    char model[41];         // Drive model string
    char serial[21];        // Drive serial number
    char firmware[9];       // Firmware version
} ahci_drive_t;

typedef struct {
    pci_device_t* pci_dev;
    hba_mem_t* abar;        // AHCI Base Memory Register
    uint32_t port_count;
    ahci_drive_t drives[32];
    uint8_t drive_count;
} ahci_controller_t;

// ===========================================
// AHCI API
// ===========================================

/**
 * Initialize AHCI controller
 * @param pci_dev PCI device for AHCI controller
 * @return Controller handle or NULL on failure
 */
ahci_controller_t* ahci_init(pci_device_t* pci_dev);

/**
 * Detect and enumerate all SATA drives
 * @param ctrl Controller handle
 * @return Number of drives found
 */
uint8_t ahci_probe_ports(ahci_controller_t* ctrl);

/**
 * Read sectors from drive
 * @param drive Drive handle
 * @param lba Starting LBA (Logical Block Address)
 * @param count Number of sectors to read
 * @param buffer Output buffer (must be count * sector_size bytes)
 * @return true on success
 */
bool ahci_read(ahci_drive_t* drive, uint64_t lba, uint32_t count, void* buffer);

/**
 * Write sectors to drive
 * @param drive Drive handle
 * @param lba Starting LBA
 * @param count Number of sectors to write
 * @param buffer Data to write (must be count * sector_size bytes)
 * @return true on success
 */
bool ahci_write(ahci_drive_t* drive, uint64_t lba, uint32_t count, const void* buffer);

/**
 * Get drive by index
 * @param ctrl Controller handle
 * @param index Drive index (0-based)
 * @return Drive handle or NULL
 */
ahci_drive_t* ahci_get_drive(ahci_controller_t* ctrl, uint8_t index);

/**
 * Find AHCI controller via PCI
 * @return PCI device or NULL if not found
 */
pci_device_t* ahci_find_controller(void);

#endif // AHCI_H
