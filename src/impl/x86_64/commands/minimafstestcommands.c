/*
 * Diagnostic Commands for MinimaFS Write Testing
 * 
 * Use these to test file writing incrementally
 */

#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "minimafshandler.h"
#include "serial.h"

// Test writing progressively larger files
void cmd_testwrite(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: testwrite <size_kb>\n");
        graphics_write_textr("Example: testwrite 64\n");
        return;
    }
    
    uint32_t size_kb = 0;
    const char* arg = argv[1];
    while (*arg >= '0' && *arg <= '9') {
        size_kb = size_kb * 10 + (*arg - '0');
        arg++;
    }
    
    if (size_kb == 0 || size_kb > 10240) {  // Max 10 MB
        graphics_write_textr("Size must be 1-10240 KB\n");
        return;
    }
    
    serial_write_str("\n=== TEST WRITE: ");
    serial_write_dec(size_kb);
    serial_write_str(" KB ===\n");
    
    // Generate test data
    uint32_t size_bytes = size_kb * 1024;
    uint8_t* data = (uint8_t*)alloc(size_bytes);
    if (!data) {
        graphics_write_textr("Failed to allocate test buffer!\n");
        return;
    }
    
    // Fill with pattern
    for (uint32_t i = 0; i < size_bytes; i++) {
        data[i] = (uint8_t)(i & 0xFF);
    }
    
    serial_write_str("Test data allocated and filled\n");
    
    // Create file
    const char* test_path = "0:/test.dat";
    
    serial_write_str("Creating file: ");
    serial_write_str(test_path);
    serial_write_str("\n");
    
    if (!minimafs_create_file(test_path, "test", "dat")) {
        graphics_write_textr("Failed to create file!\n");
        free_mem(data);
        return;
    }
    
    serial_write_str("File created, opening for write...\n");
    
    minimafs_file_handle_t* f = minimafs_open(test_path, false);
    if (!f) {
        graphics_write_textr("Failed to open file!\n");
        free_mem(data);
        return;
    }
    
    serial_write_str("Writing ");
    serial_write_dec(size_bytes);
    serial_write_str(" bytes...\n");
    
    // Write in 4KB chunks with progress
    uint32_t written = 0;
    const uint32_t CHUNK = 4096;
    
    while (written < size_bytes) {
        uint32_t to_write = size_bytes - written;
        if (to_write > CHUNK) to_write = CHUNK;
        
        uint32_t result = minimafs_write(f, data + written, to_write);
        if (result != to_write) {
            serial_write_str("Write failed at offset ");
            serial_write_dec(written);
            serial_write_str("\n");
            break;
        }
        
        written += to_write;
        
        if ((written % (64 * 1024)) == 0) {  // Every 64 KB
            serial_write_str("  ");
            serial_write_dec(written / 1024);
            serial_write_str(" KB written\n");
        }
    }
    
    minimafs_close(f);
    free_mem(data);
    
    if (written == size_bytes) {
        serial_write_str("SUCCESS: Wrote ");
        serial_write_dec(size_kb);
        serial_write_str(" KB\n");
        
        graphics_write_textr("Test write completed successfully!\n");
    } else {
        serial_write_str("FAILED: Only wrote ");
        serial_write_dec(written);
        serial_write_str(" / ");
        serial_write_dec(size_bytes);
        serial_write_str(" bytes\n");
        
        graphics_write_textr("Test write FAILED!\n");
    }
}

// Verify file was written correctly
void cmd_verifyfile(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: verifyfile <path>\n");
        return;
    }
    
    const char* path = argv[1];
    
    serial_write_str("\n=== VERIFY FILE: ");
    serial_write_str(path);
    serial_write_str(" ===\n");
    
    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) {
        graphics_write_textr("Failed to open file!\n");
        return;
    }
    
    uint32_t file_size = f->data_size;
    serial_write_str("File size: ");
    serial_write_dec(file_size);
    serial_write_str(" bytes\n");
    
    // Read and verify
    uint8_t* buffer = (uint8_t*)alloc(4096);
    if (!buffer) {
        graphics_write_textr("Out of memory!\n");
        minimafs_close(f);
        return;
    }
    
    uint32_t offset = 0;
    bool errors = false;
    
    while (offset < file_size) {
        uint32_t to_read = file_size - offset;
        if (to_read > 4096) to_read = 4096;
        
        uint32_t read = minimafs_read(f, buffer, to_read);
        if (read != to_read) {
            serial_write_str("Read error at offset ");
            serial_write_dec(offset);
            serial_write_str("\n");
            errors = true;
            break;
        }
        
        // Verify pattern
        for (uint32_t i = 0; i < read; i++) {
            uint8_t expected = (offset + i) & 0xFF;
            if (buffer[i] != expected) {
                serial_write_str("Data mismatch at offset ");
                serial_write_dec(offset + i);
                serial_write_str(": expected ");
                serial_write_hex(expected);
                serial_write_str(", got ");
                serial_write_hex(buffer[i]);
                serial_write_str("\n");
                errors = true;
                break;
            }
        }
        
        if (errors) break;
        
        offset += read;
        
        if ((offset % (128 * 1024)) == 0) {
            serial_write_str("  Verified ");
            serial_write_dec(offset / 1024);
            serial_write_str(" KB\n");
        }
    }
    
    minimafs_close(f);
    free_mem(buffer);
    
    if (!errors) {
        serial_write_str("SUCCESS: File verified!\n");
        graphics_write_textr("File verification passed!\n");
    } else {
        serial_write_str("FAILED: Verification errors found!\n");
        graphics_write_textr("File verification FAILED!\n");
    }
}

// Show disk usage
void cmd_diskusage(int argc, const char** argv) {
    minimafs_drive_t* drive = get_drive(0);
    if (!drive || !drive->mounted) {
        graphics_write_textr("Drive 0 not mounted!\n");
        return;
    }
    
    serial_write_str("\n=== DISK USAGE ===\n");
    serial_write_str("Total blocks: ");
    serial_write_dec(drive->storage_desc.total_blocks);
    serial_write_str("\n");
    serial_write_str("Used blocks:  ");
    serial_write_dec(drive->storage_desc.used_blocks);
    serial_write_str("\n");
    serial_write_str("Free blocks:  ");
    serial_write_dec(drive->storage_desc.free_blocks);
    serial_write_str("\n");
    serial_write_str("Block size:   4096 bytes\n");
    serial_write_str("Total size:   ");
    serial_write_dec(drive->storage_desc.total_size / (1024 * 1024));
    serial_write_str(" MB\n");
    serial_write_str("Used size:    ");
    serial_write_dec(drive->storage_desc.used_size / (1024 * 1024));
    serial_write_str(" MB\n");
    serial_write_str("Free size:    ");
    serial_write_dec(drive->storage_desc.free_size / (1024 * 1024));
    serial_write_str(" MB\n");
    
    uint8_t percent_used = (drive->storage_desc.used_size * 100) / drive->storage_desc.total_size;
    serial_write_str("Usage:        ");
    serial_write_dec(percent_used);
    serial_write_str("%\n");
    
    graphics_write_textr("Disk usage info written to serial\n");
}

void register_testwrite(void) {
    command_register("testwrite", cmd_testwrite);
}

void register_verifyfile(void) {
    command_register("verifyfile", cmd_verifyfile);
}

void register_diskusage(void) {
    command_register("diskusage", cmd_diskusage);
}

REGISTER_COMMAND(register_testwrite);
REGISTER_COMMAND(register_verifyfile);
REGISTER_COMMAND(register_diskusage);