#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "minimafshandler.h"
#include "x86_64/minimafs.h"
#include "serial.h"

// ===========================================
// IMPROVED FORMAT COMMAND
// ===========================================

void cmd_format_debug(int argc, const char** argv) {
    serial_write_str("\n=== FORMAT DEBUG ===\n");
    graphics_write_textr("Formatting drive...\n");
    
    minimafs_disk_device_t* device = getminimadrive();
    if (!device) {
        graphics_write_textr("ERROR: No device!\n");
        serial_write_str("ERROR: getminimadrive() returned NULL\n");
        return;
    }
    
    serial_write_str("Device OK\n");
    
    uint64_t size = 128ULL * 1024 * 1024;  // 128 MB
    
    serial_write_str("Calling minimafs_format...\n");
    if (!minimafs_format(device, size, 0, "maindisk")) {
        graphics_write_textr("ERROR: Formatting failed!\n");
        serial_write_str("ERROR: minimafs_format returned false\n");
        return;
    }
    
    graphics_write_textr("Format OK. Mounting...\n");
    serial_write_str("Format succeeded. Calling minimafs_mount...\n");
    
    // Mount immediately after format
    int mount_result = minimafs_mount(device, 0);
    if (mount_result != 1) {
        graphics_write_textr("ERROR: Mount failed! Code=");
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", mount_result);
        graphics_write_textr(buf);
        graphics_write_textr("\n");
        serial_write_str("ERROR: minimafs_mount returned ");
        serial_write_dec(mount_result);
        serial_write_str("\n");
        return;
    }
    
    graphics_write_textr("Mounted OK.\n");
    serial_write_str("Mount succeeded.\n");
    
    // Check drive is actually mounted
    minimafs_drive_t* drive = get_drive(0);
    if (!drive) {
        serial_write_str("ERROR: get_drive(0) returned NULL\n");
        graphics_write_textr("ERROR: Drive not accessible!\n");
        return;
    }
    
    if (!drive->mounted) {
        serial_write_str("ERROR: Drive 0 shows mounted=false\n");
        graphics_write_textr("ERROR: Drive not marked as mounted!\n");
        return;
    }
    
    serial_write_str("Drive 0 is mounted. Root block=");
    serial_write_dec(drive->storage_desc.root_block);
    serial_write_str("\n");
    
    // Refresh storage descriptor
    minimafs_refresh_storage_desc(drive);
    
    serial_write_str("Creating test file...\n");
    graphics_write_textr("Creating system.conf...\n");

    minimafs_mkdir("0:/");
    
    // Create a file
    minimafs_create_file("0:/system.conf", "text", "conf");

    minimafs_folder_desc_t root_desc;
    minimafs_read_folder_desc(drive, "/", &root_desc);
    graphics_write_textr("File created OK.\n");
    
    // Verify it was added to root folder
    if (!minimafs_read_folder_desc(drive, "/", &root_desc)) {
        serial_write_str("ERROR: Failed to read root folder.desc\n");
        graphics_write_textr("ERROR: Can't read root folder!\n");
        return;
    }
    
    serial_write_str("Root folder has ");
    serial_write_dec(root_desc.entry_count);
    serial_write_str(" entries\n");
    
    for (uint32_t i = 0; i < root_desc.entry_count; i++) {
        serial_write_str("  Entry ");
        serial_write_dec(i);
        serial_write_str(": ");
        serial_write_str(root_desc.entries[i].name);
        serial_write_str(" (block ");
        serial_write_dec(root_desc.entries[i].block_offset);
        serial_write_str(")\n");
    }
    
    graphics_write_textr("Format complete!\n");
    serial_write_str("=== FORMAT DEBUG DONE ===\n\n");
}

// ===========================================
// IMPROVED LISTROOT COMMAND
// ===========================================

void cmd_listroot_debug(int argc, const char** argv) {
    serial_write_str("\n=== LISTROOT DEBUG ===\n");
    
    // Check drive
    minimafs_drive_t* drive = get_drive(0);
    if (!drive) {
        serial_write_str("ERROR: get_drive(0) = NULL\n");
        graphics_write_textr("ERROR: Drive 0 not found!\n");
        return;
    }
    
    if (!drive->mounted) {
        serial_write_str("ERROR: Drive 0 not mounted\n");
        graphics_write_textr("ERROR: Drive 0 not mounted! Use 'mount' first.\n");
        return;
    }
    
    serial_write_str("Drive 0 is mounted. Root block=");
    serial_write_dec(drive->storage_desc.root_block);
    serial_write_str("\n");
    
    // Read root folder descriptor directly
    minimafs_folder_desc_t root_desc;
    if (!minimafs_read_folder_desc(drive, "/", &root_desc)) {
        serial_write_str("ERROR: Failed to read root folder.desc\n");
        graphics_write_textr("ERROR: Can't read root folder descriptor!\n");
        return;
    }
    
    serial_write_str("Root folder.desc loaded:\n");
    serial_write_str("  Path: ");
    serial_write_str(root_desc.path);
    serial_write_str("\n  Block: ");
    serial_write_dec(root_desc.block_offset);
    serial_write_str("\n  Entry count: ");
    serial_write_dec(root_desc.entry_count);
    serial_write_str("\n");
    
    if (root_desc.entry_count == 0) {
        serial_write_str("Root folder is empty!\n");
        graphics_write_textr("Root directory is empty.\n");
        return;
    }
    
    // List all entries including hidden
    serial_write_str("All entries (including hidden):\n");
    for (uint32_t i = 0; i < root_desc.entry_count; i++) {
        serial_write_str("  [");
        serial_write_dec(i);
        serial_write_str("] ");
        serial_write_str(root_desc.entries[i].name);
        serial_write_str(" - Block ");
        serial_write_dec(root_desc.entries[i].block_offset);
        serial_write_str(", Type=");
        serial_write_dec(root_desc.entries[i].type);
        serial_write_str(", Hidden=");
        serial_write_dec(root_desc.entries[i].hidden ? 1 : 0);
        serial_write_str("\n");
    }
    
    // Now use the normal API
    minimafs_dir_entry_t entries[256];
    uint32_t count = minimafs_list_dir("0:/", entries, 256);
    
    serial_write_str("minimafs_list_dir returned ");
    serial_write_dec(count);
    serial_write_str(" entries\n");
    
    graphics_write_textr("Root directory contents:\n");
    if (count == 0) {
        graphics_write_textr("  (empty)\n");
    } else {
        for (uint32_t i = 0; i < count; i++) {
            graphics_write_textr("  ");
            graphics_write_textr(entries[i].name);
            if (entries[i].type == MINIMAFS_TYPE_DIR) {
                graphics_write_textr(" [DIR]");
            }
            graphics_write_textr("\n");
        }
    }
    
    serial_write_str("=== LISTROOT DEBUG DONE ===\n\n");
}

// ===========================================
// RECURSIVE LISTDISK COMMAND
// ===========================================

void listdisk_recursive(const char* path, int indent) {
    minimafs_drive_t* drive = get_drive(0);
    if (!drive || !drive->mounted) return;

    minimafs_folder_desc_t folder;
    if (!minimafs_read_folder_desc(drive, path, &folder)) return;

    // Print folder name
    for (int i = 0; i < indent; i++) graphics_write_textr("  ");
    graphics_write_textr(path);
    graphics_write_textr("/\n");

    // Iterate entries
    for (uint32_t i = 0; i < folder.entry_count; i++) {
        minimafs_dir_entry_t* entry = &folder.entries[i];

        for (int j = 0; j < indent + 1; j++) graphics_write_textr("  ");

        graphics_write_textr(entry->name);

        if (entry->type == MINIMAFS_TYPE_DIR) {
            graphics_write_textr(" [DIR]\n");

            // Build subfolder path
            char subpath[MINIMAFS_MAX_PATH];
            if (strcmp(path, "/") == 0) {
                snprintf(subpath, sizeof(subpath), "/%s", entry->name);
            } else {
                snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->name);
            }

            // Recurse into subfolder
            listdisk_recursive(subpath, indent + 1);
        } else {
            graphics_write_textr("\n");
        }
    }
}

void cmd_listdisk(int argc, const char** argv) {
    serial_write_str("\n=== LISTDISK ===\n");

    minimafs_drive_t* drive = get_drive(0);
    if (!drive || !drive->mounted) {
        serial_write_str("ERROR: Drive 0 not mounted!\n");
        graphics_write_textr("Drive not mounted!\n");
        return;
    }

    listdisk_recursive("/", 0);

    serial_write_str("=== LISTDISK DONE ===\n\n");
    graphics_write_textr("Disk listing complete.\n");
}

void register_listdisk(void) {
    command_register("listdisk", cmd_listdisk);
}

void register_format_debug(void) {
    command_register("format", cmd_format_debug);
}

void register_listroot_debug(void) {
    command_register("listroot", cmd_listroot_debug);
}

REGISTER_COMMAND(register_format_debug);
REGISTER_COMMAND(register_listroot_debug);
REGISTER_COMMAND(register_listdisk);