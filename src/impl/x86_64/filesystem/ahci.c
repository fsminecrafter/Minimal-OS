#include "x86_64/ahci.h"
#include "serial.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/port.h"

// ===========================================
// PORT MANAGEMENT
// ===========================================

static void ahci_start_cmd(hba_port_t* port) {
    // Wait until CR (bit 15) is cleared
    while (port->cmd & (1 << 15));
    
    // Set FRE (bit 4) and ST (bit 0)
    port->cmd |= (1 << 4);
    port->cmd |= (1 << 0);

    // Wait for FR/CR to become set (engine running)
    int timeout = 1000000;
    while (timeout-- > 0) {
        if ((port->cmd & (1 << 14)) && (port->cmd & (1 << 15))) {
            break;
        }
    }
}

static void ahci_stop_cmd(hba_port_t* port) {
    // Clear ST (bit 0)
    port->cmd &= ~(1 << 0);
    
    // Clear FRE (bit 4)
    port->cmd &= ~(1 << 4);
    
    // Wait until FR (bit 14) and CR (bit 15) are cleared
    while (port->cmd & ((1 << 14) | (1 << 15)));
}

static uint32_t ahci_check_type(hba_port_t* port) {
    uint32_t ssts = port->ssts;
    
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;
    
    if (det != HBA_PORT_DET_PRESENT) return 0;
    if (ipm != HBA_PORT_IPM_ACTIVE) return 0;

    uint32_t sig = port->sig;
    if (sig == 0 || sig == 0xFFFFFFFF) {
        // Give the controller a moment to populate signature
        for (int i = 0; i < 100000; i++) {
            port_wait();
            sig = port->sig;
            if (sig != 0 && sig != 0xFFFFFFFF) {
                break;
            }
        }
    }

    if (sig == 0 || sig == 0xFFFFFFFF) {
        return 0;
    }

    return sig;
}

static int ahci_find_cmdslot(hba_port_t* port);

static bool ahci_port_reset(hba_port_t* port) {
    if (!port) return false;

    // Stop command engine before reset
    ahci_stop_cmd(port);

    // Clear errors
    port->serr = 0xFFFFFFFF;

    // Issue COMRESET (DET=1), then clear (DET=0)
    uint32_t sctl = port->sctl;
    port->sctl = (sctl & ~0x0F) | 0x01;
    for (int i = 0; i < 100000; i++) {
        port_wait();
    }
    port->sctl = (sctl & ~0x0F);

    // Wait for device to become present and active
    int timeout = 1000000;
    while (timeout-- > 0) {
        uint32_t ssts = port->ssts;
        uint8_t det = ssts & 0x0F;
        uint8_t ipm = (ssts >> 8) & 0x0F;
        if (det == HBA_PORT_DET_PRESENT && ipm == HBA_PORT_IPM_ACTIVE) {
            return true;
        }
    }

    return false;
}

static bool ahci_port_wait_ready(hba_port_t* port) {
    int timeout = 1000000;
    while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && timeout > 0) {
        timeout--;
    }
    return timeout > 0;
}

static void ahci_dump_port(const char* tag, hba_port_t* port) {
    if (!port) return;
    serial_write_str("AHCI: Port dump (");
    serial_write_str(tag);
    serial_write_str(") cmd=0x");
    serial_write_hex(port->cmd);
    serial_write_str(" is=0x");
    serial_write_hex(port->is);
    serial_write_str(" tfd=0x");
    serial_write_hex(port->tfd);
    serial_write_str(" ssts=0x");
    serial_write_hex(port->ssts);
    serial_write_str(" sctl=0x");
    serial_write_hex(port->sctl);
    serial_write_str(" serr=0x");
    serial_write_hex(port->serr);
    serial_write_str(" ci=0x");
    serial_write_hex(port->ci);
    serial_write_str(" sact=0x");
    serial_write_hex(port->sact);
    serial_write_str("\n");
}

static bool ahci_wait_engine(hba_port_t* port) {
    int timeout = 1000000;
    while (timeout-- > 0) {
        if ((port->cmd & (1 << 15)) && (port->cmd & (1 << 14))) {
            return true; // CR and FR set
        }
    }
    return false;
}

static void ahci_port_enable(hba_port_t* port) {
    if (!port) return;
    // Ensure device is powered/spun up (best effort)
    port->cmd |= (1 << 1);  // SUD
    port->cmd |= (1 << 2);  // POD
    // Ensure command engine is running and active
    if ((port->cmd & (1 << 4)) == 0 || (port->cmd & (1 << 0)) == 0 ||
        (port->cmd & (1 << 15)) == 0 || (port->cmd & (1 << 14)) == 0) {
        ahci_stop_cmd(port);
        ahci_start_cmd(port);
        if (!ahci_wait_engine(port)) {
            serial_write_str("AHCI: Command engine failed to start\n");
            ahci_dump_port("engine", port);
        }
    }
}

static bool ahci_identify(ahci_drive_t* drive) {
    if (!drive || !drive->present) return false;

    hba_port_t* port = drive->port;
    ahci_port_enable(port);

    // DMA-safe buffer for IDENTIFY data (512 bytes)
    uint16_t* id_buf = (uint16_t*)alloc_page_zeroed();
    if (!id_buf) return false;

    port->is = 0xFFFFFFFF;
    port->serr = 0xFFFFFFFF;

    int slot = ahci_find_cmdslot(port);
    if (slot == -1) return false;

    hba_cmd_header_t* cmdheader = (hba_cmd_header_t*)((uintptr_t)port->clb | ((uint64_t)port->clbu << 32));
    cmdheader += slot;
    memset(cmdheader, 0, sizeof(hba_cmd_header_t));
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 0;
    cmdheader->prdtl = 1;
    cmdheader->prdbc = 0;

    hba_cmd_tbl_t* cmdtbl = (hba_cmd_tbl_t*)((uintptr_t)cmdheader->ctba | ((uint64_t)cmdheader->ctbau << 32));
    memset(cmdtbl, 0, sizeof(hba_cmd_tbl_t));

    cmdtbl->prdt_entry[0].dba = ((uintptr_t)id_buf) & 0xFFFFFFFF;
    cmdtbl->prdt_entry[0].dbau = ((uintptr_t)id_buf >> 32) & 0xFFFFFFFF;
    cmdtbl->prdt_entry[0].dbc = 512 - 1;
    cmdtbl->prdt_entry[0].i = 1;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)(cmdtbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_IDENTIFY;
    cmdfis->device = 0;
    cmdfis->countl = 1;
    cmdfis->counth = 0;

    if (!ahci_port_wait_ready(port)) {
        serial_write_str("AHCI: IDENTIFY port busy timeout\n");
        ahci_dump_port("identify-timeout", port);
        return false;
    }

    port->ci = 1 << slot;

    int timeout = 1000000;
    while (true) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            serial_write_str("AHCI: IDENTIFY task file error\n");
            ahci_dump_port("identify-tfe", port);
            return false;
        }
        if (--timeout == 0) {
            serial_write_str("AHCI: IDENTIFY timeout\n");
            ahci_dump_port("identify-timeout2", port);
            return false;
        }
    }

    if (port->is & (1 << 30)) {
        serial_write_str("AHCI: IDENTIFY error\n");
        ahci_dump_port("identify-error", port);
        return false;
    }

    if (cmdheader->prdbc == 0) {
        serial_write_str("AHCI: IDENTIFY transferred 0 bytes\n");
        ahci_dump_port("identify-prdbc0", port);
        return false;
    }

    // Debug: dump a few IDENTIFY words
    serial_write_str("AHCI: IDENTIFY prdbc=");
    serial_write_dec(cmdheader->prdbc);
    serial_write_str(" w0=0x");
    serial_write_hex(id_buf[0]);
    serial_write_str(" w1=0x");
    serial_write_hex(id_buf[1]);
    serial_write_str(" w60=0x");
    serial_write_hex(id_buf[60]);
    serial_write_str(" w61=0x");
    serial_write_hex(id_buf[61]);
    serial_write_str(" w83=0x");
    serial_write_hex(id_buf[83]);
    serial_write_str(" w100=0x");
    serial_write_hex(id_buf[100]);
    serial_write_str(" w101=0x");
    serial_write_hex(id_buf[101]);
    serial_write_str("\n");

    // Parse model (words 27-46, 40 chars, byte-swapped per word)
    char model[41];
    for (int i = 0; i < 20; i++) {
        uint16_t w = id_buf[27 + i];
        model[i * 2] = (char)(w >> 8);
        model[i * 2 + 1] = (char)(w & 0xFF);
    }
    model[40] = '\0';
    // Trim trailing spaces
    for (int i = 39; i >= 0; i--) {
        if (model[i] == ' ' || model[i] == '\0') {
            model[i] = '\0';
        } else {
            break;
        }
    }
    if (model[0] != '\0') {
        strncpy(drive->model, model, sizeof(drive->model) - 1);
        drive->model[sizeof(drive->model) - 1] = '\0';
    }

    // Parse sector count
    uint64_t sectors = 0;
    if (id_buf[83] & (1 << 10)) {
        sectors = ((uint64_t)id_buf[100]) |
                  ((uint64_t)id_buf[101] << 16) |
                  ((uint64_t)id_buf[102] << 32) |
                  ((uint64_t)id_buf[103] << 48);
    } else {
        sectors = (uint32_t)id_buf[60] | ((uint32_t)id_buf[61] << 16);
    }
    drive->sectors = sectors;
    drive->sector_size = 512;

    return true;
}

// ===========================================
// INITIALIZATION
// ===========================================

pci_device_t* ahci_find_controller(void) {
    extern pci_device_t pci_devices[];
    extern int pci_device_count;
    
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* dev = &pci_devices[i];
        if (dev->class_code == AHCI_CLASS_CODE && 
            dev->subclass == AHCI_SUBCLASS &&
            dev->prog_if == AHCI_PROG_IF) {
            return dev;
        }
    }
    
    return NULL;
}

ahci_controller_t* ahci_init(pci_device_t* pci_dev) {
    if (!pci_dev) {
        serial_write_str("AHCI: No PCI device provided\n");
        return NULL;
    }
    
    serial_write_str("AHCI: Initializing controller...\n");
    serial_write_str("AHCI: Vendor=0x");
    serial_write_hex(pci_dev->vendor_id);
    serial_write_str(" Device=0x");
    serial_write_hex(pci_dev->device_id);
    serial_write_str("\n");
    
    // Enable bus mastering
    pci_enable_io_busmaster(pci_dev);
    
    // Get ABAR (BAR5)
    if (pci_dev->bar_type[5] != PCI_BAR_MEM) {
        serial_write_str("AHCI: BAR5 is not memory mapped\n");
        return NULL;
    }
    
    uintptr_t abar_phys = pci_dev->bar[5];
    serial_write_str("AHCI: ABAR physical = 0x");
    serial_write_hex(abar_phys);
    serial_write_str("\n");
    
    // Map ABAR to virtual memory
    hba_mem_t* abar = (hba_mem_t*)pci_map_bar_mmio(pci_dev, 5);
    serial_write_str("AHCI: ABAR virtual  = 0x");
    serial_write_hex((uintptr_t)abar);
    serial_write_str("\n");
    
    // Allocate controller structure
    ahci_controller_t* ctrl = (ahci_controller_t*)alloc(sizeof(ahci_controller_t));
    if (!ctrl) {
        serial_write_str("AHCI: Failed to allocate controller\n");
        return NULL;
    }
    
    memset(ctrl, 0, sizeof(ahci_controller_t));
    ctrl->pci_dev = pci_dev;
    ctrl->abar = abar;
    
    // Take control from BIOS
    if (abar->cap2 & (1 << 0)) {  // BIOS/OS Handoff supported
        abar->bohc |= (1 << 1);    // Request OS ownership
        
        // Wait for BIOS to release
        int timeout = 1000;
        while ((abar->bohc & (1 << 0)) && timeout > 0) {
            timeout--;
        }
        
        if (timeout == 0) {
            serial_write_str("AHCI: BIOS handoff timeout\n");
        }
    }
    
    // Enable AHCI mode
    abar->ghc |= (1 << 31);  // AHCI Enable
    
    // Get number of ports
    uint32_t pi = abar->pi;
    ctrl->port_count = 0;
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            ctrl->port_count++;
        }
    }
    
    serial_write_str("AHCI: Found ");
    serial_write_dec(ctrl->port_count);
    serial_write_str(" ports\n");
    
    return ctrl;
}

// ===========================================
// PORT PROBING
// ===========================================

static void ahci_port_rebase(ahci_controller_t* ctrl, hba_port_t* port, int portno) {
    ahci_stop_cmd(port);
    
    // Command list (1KB per port) - allocate DMA-safe physical page
    uintptr_t clb = (uintptr_t)alloc_page_zeroed();
    memset((void*)clb, 0, 1024);
    port->clb = clb & 0xFFFFFFFF;
    port->clbu = (clb >> 32) & 0xFFFFFFFF;
    
    // FIS receive area (256 bytes per port) - DMA-safe physical page
    uintptr_t fb = (uintptr_t)alloc_page_zeroed();
    memset((void*)fb, 0, 256);
    port->fb = fb & 0xFFFFFFFF;
    port->fbu = (fb >> 32) & 0xFFFFFFFF;
    
    // Command tables (256 bytes per command * 32 commands)
    hba_cmd_header_t* cmdheader = (hba_cmd_header_t*)clb;
    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 8;  // 8 PRDT entries per command table
        
        // Command table must be 128-byte aligned and DMA-safe
        uintptr_t cmdtbl = (uintptr_t)alloc_page_zeroed();
        memset((void*)cmdtbl, 0, sizeof(hba_cmd_tbl_t) + 7 * sizeof(hba_prdt_entry_t));
        
        cmdheader[i].ctba = cmdtbl & 0xFFFFFFFF;
        cmdheader[i].ctbau = (cmdtbl >> 32) & 0xFFFFFFFF;
    }

    // Clear errors/interrupts and ensure device power/spin-up
    port->is = 0xFFFFFFFF;
    port->serr = 0xFFFFFFFF;
    port->cmd |= (1 << 1);  // SUD
    port->cmd |= (1 << 2);  // POD

    if (!ahci_port_reset(port)) {
        serial_write_str("AHCI: Port reset failed on port ");
        serial_write_dec(portno);
        serial_write_str("\n");
    }

    ahci_start_cmd(port);
}

uint8_t ahci_probe_ports(ahci_controller_t* ctrl) {
    if (!ctrl) return 0;
    
    serial_write_str("AHCI: Probing ports...\n");
    
    uint32_t pi = ctrl->abar->pi;
    ctrl->drive_count = 0;
    
    for (int i = 0; i < 32; i++) {
        if ((pi & (1 << i)) == 0) continue;
        
        hba_port_t* port = &ctrl->abar->ports[i];
        uint32_t sig = ahci_check_type(port);
        if (sig == 0) {
            // Try a port reset to coax signature on some controllers
            if (ahci_port_reset(port)) {
                sig = ahci_check_type(port);
            }
        }
        
        if (sig == 0) continue;
        
        serial_write_str("AHCI: Port ");
        serial_write_dec(i);
        serial_write_str(" - Signature: 0x");
        serial_write_hex(sig);
        serial_write_str("\n");
        
        if (sig == SATA_SIG_ATA) {
            serial_write_str("AHCI: Found SATA drive on port ");
            serial_write_dec(i);
            serial_write_str("\n");
            
            ahci_drive_t* drive = &ctrl->drives[ctrl->drive_count++];
            drive->present = true;
            drive->port_num = i;
            drive->port = port;
            drive->signature = sig;
            drive->sector_size = 512;  // Default sector size
            
            // Rebase port
            ahci_port_rebase(ctrl, port, i);
            
            // Identify drive
            bool id_ok = ahci_identify(drive);
            if (!id_ok || drive->sectors == 0) {
                serial_write_str("AHCI: IDENTIFY failed or returned 0 sectors on port ");
                serial_write_dec(i);
                serial_write_str("\n");
                drive->sectors = 0;
                strcpy(drive->model, "SATA Drive");
            }
            strcpy(drive->serial, "Unknown");
            strcpy(drive->firmware, "0.0");
        }
        else if (sig == SATA_SIG_ATAPI) {
            serial_write_str("AHCI: Found SATAPI drive on port ");
            serial_write_dec(i);
            serial_write_str(" (not supported)\n");
        }
    }
    
    serial_write_str("AHCI: Found ");
    serial_write_dec(ctrl->drive_count);
    serial_write_str(" SATA drives\n");
    
    return ctrl->drive_count;
}

ahci_drive_t* ahci_get_drive(ahci_controller_t* ctrl, uint8_t index) {
    if (!ctrl || index >= ctrl->drive_count) return NULL;
    return &ctrl->drives[index];
}

// ===========================================
// I/O OPERATIONS
// ===========================================

static int ahci_find_cmdslot(hba_port_t* port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & (1 << i)) == 0) {
            return i;
        }
    }
    return -1;
}

bool ahci_read(ahci_drive_t* drive, uint64_t lba, uint32_t count, void* buffer) {
    if (!drive || !drive->present || !buffer) return false;
    
    hba_port_t* port = drive->port;
    ahci_port_enable(port);
    
    // Clear interrupt status
    port->is = 0xFFFFFFFF;
    port->serr = 0xFFFFFFFF;
    
    // Find free command slot
    int slot = ahci_find_cmdslot(port);
    if (slot == -1) {
        serial_write_str("AHCI: No free command slots\n");
        return false;
    }
    
    hba_cmd_header_t* cmdheader = (hba_cmd_header_t*)((uintptr_t)port->clb | ((uint64_t)port->clbu << 32));
    cmdheader += slot;
    memset(cmdheader, 0, sizeof(hba_cmd_header_t));
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 0;  // Read
    cmdheader->prdtl = 1;  // One PRDT entry
    cmdheader->prdbc = 0;
    
    hba_cmd_tbl_t* cmdtbl = (hba_cmd_tbl_t*)((uintptr_t)cmdheader->ctba | ((uint64_t)cmdheader->ctbau << 32));
    memset(cmdtbl, 0, sizeof(hba_cmd_tbl_t));
    
    // Setup PRDT
    cmdtbl->prdt_entry[0].dba = ((uintptr_t)buffer) & 0xFFFFFFFF;
    cmdtbl->prdt_entry[0].dbau = ((uintptr_t)buffer >> 32) & 0xFFFFFFFF;
    cmdtbl->prdt_entry[0].dbc = (count * drive->sector_size) - 1;
    cmdtbl->prdt_entry[0].i = 1;  // Interrupt on completion
    
    // Setup command FIS
    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)(cmdtbl->cfis);
    cmdfis->fis_type = 0x27;  // Register H2D
    cmdfis->c = 1;  // Command
    cmdfis->command = ATA_CMD_READ_DMA_EX;
    
    cmdfis->lba0 = lba & 0xFF;
    cmdfis->lba1 = (lba >> 8) & 0xFF;
    cmdfis->lba2 = (lba >> 16) & 0xFF;
    cmdfis->device = 1 << 6;  // LBA mode
    
    cmdfis->lba3 = (lba >> 24) & 0xFF;
    cmdfis->lba4 = (lba >> 32) & 0xFF;
    cmdfis->lba5 = (lba >> 40) & 0xFF;
    
    cmdfis->countl = count & 0xFF;
    cmdfis->counth = (count >> 8) & 0xFF;
    
    // Wait for port to be ready
    if (!ahci_port_wait_ready(port)) {
        serial_write_str("AHCI: Port busy timeout\n");
        ahci_dump_port("read-busy", port);
        return false;
    }
    
    // Issue command
    port->ci = 1 << slot;
    
    // Wait for completion
    int timeout = 1000000;
    while (true) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {  // Task file error
            serial_write_str("AHCI: Task file error\n");
            ahci_dump_port("read-tfe", port);
            return false;
        }
        if (--timeout == 0) {
            serial_write_str("AHCI: Read timeout\n");
            ahci_dump_port("read-timeout", port);
            return false;
        }
    }
    
    // Check for errors
    if (port->is & (1 << 30)) {
        serial_write_str("AHCI: Read error\n");
        ahci_dump_port("read-error", port);
        return false;
    }

    if (cmdheader->prdbc == 0) {
        serial_write_str("AHCI: Read transferred 0 bytes\n");
        ahci_dump_port("read-prdbc0", port);
        return false;
    }
    
    return true;
}

bool ahci_write(ahci_drive_t* drive, uint64_t lba, uint32_t count, const void* buffer) {
    if (!drive || !drive->present || !buffer) return false;
    
    // Similar to read but with ATA_CMD_WRITE_DMA_EX and w=1
    // Implementation similar to ahci_read() but with write command
    
    hba_port_t* port = drive->port;
    ahci_port_enable(port);
    port->is = 0xFFFFFFFF;
    port->serr = 0xFFFFFFFF;
    
    int slot = ahci_find_cmdslot(port);
    if (slot == -1) return false;
    
    hba_cmd_header_t* cmdheader = (hba_cmd_header_t*)((uintptr_t)port->clb | ((uint64_t)port->clbu << 32));
    cmdheader += slot;
    memset(cmdheader, 0, sizeof(hba_cmd_header_t));
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 1;  // Write
    cmdheader->prdtl = 1;
    cmdheader->prdbc = 0;
    
    hba_cmd_tbl_t* cmdtbl = (hba_cmd_tbl_t*)((uintptr_t)cmdheader->ctba | ((uint64_t)cmdheader->ctbau << 32));
    memset(cmdtbl, 0, sizeof(hba_cmd_tbl_t));
    
    cmdtbl->prdt_entry[0].dba = ((uintptr_t)buffer) & 0xFFFFFFFF;
    cmdtbl->prdt_entry[0].dbau = ((uintptr_t)buffer >> 32) & 0xFFFFFFFF;
    cmdtbl->prdt_entry[0].dbc = (count * drive->sector_size) - 1;
    cmdtbl->prdt_entry[0].i = 1;
    
    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)(cmdtbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_WRITE_DMA_EX;
    
    cmdfis->lba0 = lba & 0xFF;
    cmdfis->lba1 = (lba >> 8) & 0xFF;
    cmdfis->lba2 = (lba >> 16) & 0xFF;
    cmdfis->device = 1 << 6;
    
    cmdfis->lba3 = (lba >> 24) & 0xFF;
    cmdfis->lba4 = (lba >> 32) & 0xFF;
    cmdfis->lba5 = (lba >> 40) & 0xFF;
    
    cmdfis->countl = count & 0xFF;
    cmdfis->counth = (count >> 8) & 0xFF;
    
    if (!ahci_port_wait_ready(port)) return false;
    
    port->ci = 1 << slot;
    
    int timeout = 1000000;
    while (true) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            ahci_dump_port("write-tfe", port);
            return false;
        }
        if (--timeout == 0) {
            ahci_dump_port("write-timeout", port);
            return false;
        }
    }
    
    if (cmdheader->prdbc == 0) {
        serial_write_str("AHCI: Write transferred 0 bytes\n");
        ahci_dump_port("write-prdbc0", port);
        return false;
    }

    return true;
}
