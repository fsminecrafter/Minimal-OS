#include "serial.h"
#include "x86_64/ahci.h"
#include "x86_64/minimafs.h"
#include "minimafshandler.h"
#include "x86_64/exec_trace.h"

#include <stdbool.h>

// Globals

pci_device_t* ahci_pci;
ahci_controller_t* ahci;
uint8_t drive_count;
ahci_drive_t* disk;
minimafs_disk_device_t* device;

void exampleinit(void) {
    serial_write_str("=== Initializing Storage System ===\n");
    
    // Find AHCI controller via PCI
    ahci_pci = ahci_find_controller();
    
    if (!ahci_pci) {
        serial_write_str("ERROR: No AHCI controller found!\n");
        serial_write_str("Your system may not have SATA support.\n");
        return;
    }
    
    // Initialize AHCI controller
    ahci = ahci_init(ahci_pci);
    
    if (!ahci) {
        serial_write_str("ERROR: Failed to initialize AHCI\n");
        return;
    }
    
    // Probe for drives
    drive_count = ahci_probe_ports(ahci);
    
    serial_write_str("Found ");
    serial_write_dec(drive_count);
    serial_write_str(" SATA drive(s)\n");
    
    if (drive_count == 0) {
        serial_write_str("ERROR: No SATA drives detected!\n");
        return;
    }
    
    // Get first drive
    ahci_drive_t* disk = ahci_get_drive(ahci, 0);
    if (!disk) {
        serial_write_str("ERROR: Failed to get drive 0\n");
        return;
    }
    
    serial_write_str("Disk 0:\n");
    serial_write_str("  Model: ");
    serial_write_str(disk->model);
    serial_write_str("\n  Sectors: ");
    serial_write_dec(disk->sectors);
    serial_write_str("\n  Sector size: ");
    serial_write_dec(disk->sector_size);
    serial_write_str(" bytes\n");
    
    // ===========================================
    // STEP 2: Initialize MinimaFS
    // ===========================================
    
    minimafs_init();
    
    // Create disk device wrapper
    
    minimafs_disk_device_t* device = (minimafs_disk_device_t*)alloc(sizeof(minimafs_disk_device_t));
    if (!device) {
        serial_write_str("ERROR: Failed to allocate MinimaFS device wrapper\n");
        return;
    }
    device->ahci_drive = disk;
    device->sector_size = disk->sector_size;
    
    // ===========================================
    // STEP 3: Format Drive (if first time)
    // ===========================================
    
    // WARNING: This erases all data!
    // Only do this on first setup or if you want to reformat
    
    bool format_drive = true;  // Set to true to format
    
    if (format_drive) {
        serial_write_str("Formatting drive...\n");
        
        uint64_t size = 128ULL * 1024 * 1024;  // 128 MB for now
        minimafs_format(device, size, 1, "maindisk");
        
        serial_write_str("Format complete!\n");
    }
    
    // ===========================================
    // STEP 4: Mount Filesystem
    // ===========================================
    
    if (!minimafs_mount(device, 1)) {
        serial_write_str("ERROR: Failed to mount filesystem\n");
        return;
    }
    
    serial_write_str("Filesystem mounted as drive 1:\n");
    
    // ===========================================
    // STEP 5: Create Directory Structure
    // ===========================================
    
    minimafs_mkdir("1:/");
    minimafs_mkdir("1:/programs");
    minimafs_mkdir("1:/etc");
    minimafs_mkdir("1:/home");
    minimafs_mkdir("1:/tmp");
    
    serial_write_str("Directory structure created\n");
    
    // ===========================================
    // STEP 6: Create and Write a File
    // ===========================================
    
    minimafs_create_file("1:/etc/system.conf", "text", "conf");
    
    minimafs_file_handle_t* f = minimafs_open("1:/etc/system.conf", false);
    
    const char* config = 
        "# System Configuration\n"
        "hostname=MinimalOS\n"
        "version=1.0\n"
        "boot_timeout=5\n";
    
    minimafs_write(f, config, strlen(config));
    minimafs_close(f);
    
    serial_write_str("Created /etc/system.conf\n");
    
    // ===========================================
    // STEP 7: Read File Back
    // ===========================================
    
    f = minimafs_open("1:/etc/system.conf", true);
    
    char buffer[1024];
    uint32_t bytes = minimafs_read(f, buffer, sizeof(buffer));
    buffer[bytes] = '\0';
    
    serial_write_str("Read from file:\n");
    serial_write_str(buffer);
    serial_write_str("\n");
    
    minimafs_close(f);
    
    // ===========================================
    // STEP 8: List Directory
    // ===========================================
    
    minimafs_dir_entry_t entries[256];
    uint32_t count = minimafs_list_dir("1:/", entries, 256);
    
    serial_write_str("Root directory contents:\n");
    for (uint32_t i = 0; i < count; i++) {
        serial_write_str("  ");
        serial_write_str(entries[i].name);
        if (entries[i].type == MINIMAFS_TYPE_DIR) {
            serial_write_str(" [DIR]");
        }
        serial_write_str("\n");
    }
    
    serial_write_str("=== Storage System Ready! ===\n");
}

minimafs_disk_device_t* initializeminimafs(int driveindex) {
    serial_write_str("=== Initializing Storage System ===\n");
    
    // Find AHCI controller via PCI
    ahci_pci = ahci_find_controller();
    
    if (!ahci_pci) {
        serial_write_str("ERROR: No AHCI controller found!\n");
        serial_write_str("Your system may not have SATA support.\n");
        return NULL;
    }
    
    // Initialize AHCI controller
    ahci = ahci_init(ahci_pci);
    
    if (!ahci) {
        serial_write_str("ERROR: Failed to initialize AHCI\n");
        return NULL;
    }
    
    // Probe for drives
    drive_count = ahci_probe_ports(ahci);
    
    serial_write_str("Found ");
    serial_write_dec(drive_count);
    serial_write_str(" SATA drive(s)\n");
    
    if (drive_count == 0) {
        serial_write_str("ERROR: No SATA drives detected!\n");
        return NULL;
    }
    
    // Get first drive
    disk = ahci_get_drive(ahci, 0);
    if (!disk) {
        serial_write_str("ERROR: Failed to get drive 0\n");
        return NULL;
    }
    
    serial_write_str("Disk 0:\n");
    serial_write_str("  Model: ");
    serial_write_str(disk->model);
    serial_write_str("\n  Sectors: ");
    serial_write_dec(disk->sectors);
    serial_write_str("\n  Sector size: ");
    serial_write_dec(disk->sector_size);
    serial_write_str(" bytes\n");
    
    minimafs_init();
    
    device = (minimafs_disk_device_t*)alloc(sizeof(minimafs_disk_device_t));
    if (!device) {
        serial_write_str("ERROR: Failed to allocate MinimaFS device wrapper\n");
        return;
    }
    device->ahci_drive = disk;
    device->sector_size = disk->sector_size;

    trace_sti(__FILE__, "initializeminimafs", __LINE__);

    return device;
}

minimafs_disk_device_t* setdrive(ahci_drive_t* disk) {
    device = (minimafs_disk_device_t*)alloc(sizeof(minimafs_disk_device_t));
    if (!device) {
        serial_write_str("ERROR: Failed to allocate MinimaFS device wrapper\n");
        return;
    }
    device->ahci_drive = disk;
    device->sector_size = disk->sector_size;
    return device;
}

int mountdrive(minimafs_disk_device_t* device, int deviceindex) {
    int successcode = minimafs_mount(device, deviceindex);
    if (!successcode == 1) {
        serial_write_str("ERROR: Failed to mount filesystem\n");
        return 0;
    }
    
    serial_write_str("Filesystem mounted as drive ");
    serial_write_dec(deviceindex);
    serial_write_str(": \n");
    trace_sti(__FILE__, "mountdrive", __LINE__);
    return successcode;
}

minimafs_disk_device_t* getminimadrive() {
    if (device) {
        serial_write_str("Returning device\n");
        return device;
    } else {
        serial_write_str("No device\n");
        return NULL;
    }
}

minimafs_disk_device_t* getahci() {
    if (device) {
        return device;
    } else {
        return NULL;
    }
}

ahci_drive_t* getadrive() {
    return disk;
}