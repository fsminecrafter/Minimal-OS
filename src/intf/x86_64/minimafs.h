#ifndef MINIMAFS_H
#define MINIMAFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * MinimaFS - MinimalOS Filesystem
 * 
 * Features:
 * - Tree structure with directory descriptors
 * - 4KB alignment for efficient storage
 * - Storage devices mounted as 1:, 2:, 3:, etc.
 * - Tagged file format with metadata
 * - Hidden folder.desc and storage.desc files
 * - Support for executable files
 */

// ===========================================
// CONSTANTS
// ===========================================

#define MINIMAFS_BLOCK_SIZE         4096        // 4KB alignment
#define MINIMAFS_MAX_FILENAME       256         // Max filename length (metadata)
#define MINIMAFS_MAX_ENTRY_NAME     64          // Max name in dir entry (keep struct small)
#define MINIMAFS_MAX_PATH           1024        // Max path length
#define MINIMAFS_MAX_DRIVES         99          // 0: through 99:
#define MINIMAFS_MAX_ROOT_ENTRIES   64          // Max entries per folder
#define MINIMAFS_MAGIC              0x4D494E46  // "MINF"

#define MINIMAFS_FOLDER_DESC        "folder.desc"
#define MINIMAFS_STORAGE_DESC       "storage.desc"

// ===========================================
// FILE TYPES
// ===========================================

typedef enum {
    MINIMAFS_TYPE_FILE = 0,
    MINIMAFS_TYPE_DIR,
    MINIMAFS_TYPE_EXECUTABLE,
    MINIMAFS_TYPE_SYMLINK
} minimafs_filetype_t;

// ===========================================
// FILE METADATA
// ===========================================

typedef struct {
    char filename[MINIMAFS_MAX_FILENAME];       // File name
    char filetype[64];                          // text, binary, executable, etc.
    char fileformat[16];                        // txt, bin, elf, etc.
    
    uint32_t file_length;                       // Total length including header
    uint32_t data_length;                       // Data length (excluding header)
    
    char created_date[32];                      // Creation date string
    char last_changed[32];                      // Last modification date
    
    char parent_folder[MINIMAFS_MAX_PATH];      // Parent directory path
    
    bool runnable;                              // Is executable?
    uint64_t entrypoint;                        // Entry point address (if runnable)
    char run_with[MINIMAFS_MAX_PATH];           // Interpreter path (if no entrypoint)
    
    bool hidden;                                // Hidden file?
    
    uint32_t block_offset;                      // Block offset on disk
    uint32_t block_count;                       // Number of blocks
} minimafs_file_metadata_t;

// ===========================================
// DIRECTORY ENTRY
// ===========================================

typedef struct {
    char name[MINIMAFS_MAX_ENTRY_NAME];         // Entry name (short)
    minimafs_filetype_t type;                   // File or directory
    uint32_t block_offset;                      // Where entry starts on disk
    uint32_t block_count;                       // Number of blocks
    bool hidden;                                // Is hidden?
} minimafs_dir_entry_t;

// ===========================================
// DIRECTORY DESCRIPTOR
// ===========================================

typedef struct {
    char path[MINIMAFS_MAX_PATH];
    uint32_t block_offset;
    uint32_t entry_count;
    minimafs_dir_entry_t entries[MINIMAFS_MAX_ROOT_ENTRIES]; // inline array
} minimafs_folder_desc_t;

// ===========================================
// STORAGE DESCRIPTOR
// ===========================================

typedef struct {
    uint32_t magic;                             // Magic number (0x4D494E46)
    
    char drive_name[64];                        // Drive name ("1", "mydrive", etc.)
    uint8_t drive_number;                       // Drive number (1-99)
    
    char password[64];                          // Password (empty if none)
    bool password_protected;                    // Is password required?
    
    uint64_t total_size;                        // Total size in bytes
    uint64_t used_size;                         // Used space in bytes
    uint64_t free_size;                         // Free space in bytes
    
    uint32_t total_blocks;                      // Total 4KB blocks
    uint32_t used_blocks;                       // Used blocks
    uint32_t free_blocks;                       // Free blocks
    uint32_t root_block;                        // Root folder.desc block
    
    uint32_t root_entries;                      // Number of root entries
    minimafs_dir_entry_t entries[MINIMAFS_MAX_ROOT_ENTRIES];
    char filesystem_label[64];                  // Volume label
    
    char created_date[32];                      // Creation date
    char last_mounted[32];                      // Last mount date
} minimafs_storage_desc_t;

// ===========================================
// MOUNTED DRIVE
// ===========================================

typedef struct {
    bool mounted;                               // Is drive mounted?
    uint8_t drive_number;                       // 1-99
    char drive_name[64];                        // "1" or "mydrive"
    
    minimafs_storage_desc_t storage_desc;       // Storage descriptor
    
    void* device_handle;                        // Device driver handle
    uint64_t device_offset;                     // Offset on physical device
    
    // Cached data
    minimafs_folder_desc_t root_folder;         // Root directory cache
} minimafs_drive_t;

// ===========================================
// FILE HANDLE
// ===========================================

typedef struct {
    bool open;                                  // Is file open?
    uint8_t drive_number;                       // Which drive
    
    char path[MINIMAFS_MAX_PATH];               // Full path
    minimafs_file_metadata_t metadata;          // File metadata
    
    uint8_t* data;                              // File data (in memory)
    uint32_t data_size;                         // Size of data
    uint32_t position;                          // Current read/write position
    
    bool modified;                              // Has file been modified?
    bool read_only;                             // Read-only mode?
    
    // Streaming fields (for large files)
    bool use_streaming;                         // Use streaming instead of loading all data?
    uint32_t file_block_offset;                 // Where the file's blocks start on disk
    uint32_t file_block_count;                  // Number of blocks for this file
    uint32_t data_offset_in_blocks;             // Byte offset of data section from block start
    uint8_t* stream_cache;                      // Cached block buffer (1 block = 4KB)
    uint32_t cached_block_index;                // Which block is in stream_cache (-1 = invalid)
} minimafs_file_handle_t;

// ===========================================
// CORE API
// ===========================================

/**
 * Initialize MinimaFS system
 */
void minimafs_init(void);

/**
 * Format a storage device with MinimaFS
 * @param device_handle Device driver handle
 * @param size Total size in bytes
 * @param drive_number Drive number (1-99)
 * @param drive_name Optional drive name (or NULL for default)
 * @return true on success
 */
bool minimafs_format(void* device_handle, uint64_t size, 
                     uint8_t drive_number, const char* drive_name);

/**
 * Mount a MinimaFS drive
 * @param device_handle Device driver handle
 * @param drive_number Drive number (1-99)
 * @return true on success
 */
int minimafs_mount(void* device_handle, uint8_t drive_number);

/**
 * Unmount a drive
 * @param drive_number Drive number
 * @return true on success
 */
bool minimafs_unmount(uint8_t drive_number);

// ===========================================
// FILE OPERATIONS
// ===========================================

/**
 * Open a file
 * @param path Full path (e.g., "1:/etc/config.txt")
 * @param read_only Open in read-only mode?
 * @return File handle or NULL on failure
 */
minimafs_file_handle_t* minimafs_open(const char* path, bool read_only);

/**
 * Close a file
 * @param handle File handle
 */
void minimafs_close(minimafs_file_handle_t* handle);

/**
 * Read from file
 * @param handle File handle
 * @param buffer Destination buffer
 * @param size Number of bytes to read
 * @return Number of bytes read
 */
uint32_t minimafs_read(minimafs_file_handle_t* handle, void* buffer, uint32_t size);

/**
 * Write to file
 * @param handle File handle
 * @param buffer Source buffer
 * @param size Number of bytes to write
 * @return Number of bytes written
 */
uint32_t minimafs_write(minimafs_file_handle_t* handle, const void* buffer, uint32_t size);

/**
 * Seek to position in file
 * @param handle File handle
 * @param offset offset in bytes
 */
bool minimafs_seek(minimafs_file_handle_t* handle, uint32_t offset);

/**
 * Create a new file
 * @param path Full path
 * @param filetype File type string (e.g., "text")
 * @param fileformat File format (e.g., "txt")
 * @return true on success
 */
bool minimafs_create_file(const char* path, const char* filetype, const char* fileformat);

/**
 * Delete a file
 * @param path Full path
 * @return true on success
 */
bool minimafs_delete_file(const char* path);

// ===========================================
// DIRECTORY OPERATIONS
// ===========================================

/**
 * Create a directory
 * @param path Full path
 * @return true on success
 */
bool minimafs_mkdir(const char* path);

/**
 * Remove a directory
 * @param path Full path
 * @return true on success
 */
bool minimafs_rmdir(const char* path);

/**
 * List directory contents
 * @param path Directory path
 * @param entries Output buffer for entries
 * @param max_entries Maximum entries to return
 * @return Number of entries found
 */
uint32_t minimafs_list_dir(const char* path, minimafs_dir_entry_t* entries, uint32_t max_entries);

/**
 * Check if path exists
 * @param path Full path
 * @return true if exists
 */
bool minimafs_exists(const char* path);

/**
 * Check if path is a directory
 * @param path Full path
 * @return true if directory
 */
bool minimafs_is_dir(const char* path);

// ===========================================
// UTILITY FUNCTIONS
// ===========================================

/**
 * Get file metadata
 * @param path Full path
 * @param metadata Output metadata structure
 * @return true on success
 */
bool minimafs_get_metadata(const char* path, minimafs_file_metadata_t* metadata);

/**
 * Set file metadata
 * @param path Full path
 * @param metadata Metadata to set
 * @return true on success
 */
bool minimafs_set_metadata(const char* path, const minimafs_file_metadata_t* metadata);

/**
 * Get storage descriptor
 * @param drive_number Drive number
 * @param desc Output descriptor
 * @return true on success
 */
bool minimafs_get_storage_desc(uint8_t drive_number, minimafs_storage_desc_t* desc);
bool minimafs_read_folder_desc(minimafs_drive_t* drive, const char* path,
                               minimafs_folder_desc_t* desc);
bool minimafs_write_folder_desc(minimafs_drive_t* drive, minimafs_folder_desc_t* desc);
bool minimafs_write_file_segments(const char* path,
                                  const void* seg1, uint32_t seg1_len,
                                  const void* seg2, uint32_t seg2_len,
                                  const char* filetype, const char* fileformat);
bool minimafs_append_file(const char* path, const void* data, uint32_t data_len);

/**
 * Parse path into drive and local path
 * @param path Full path (e.g., "1:/etc/config.txt")
 * @param drive_number Output drive number
 * @param local_path Output local path (e.g., "/etc/config.txt")
 * @return true on success
 */
bool minimafs_parse_path(const char* path, uint8_t* drive_number, char* local_path);

/**
 * Get current date/time string
 * @param buffer Output buffer
 * @param size Buffer size
 */
void minimafs_get_datetime(char* buffer, size_t size);

minimafs_drive_t* get_drive(uint8_t drive_number);
void minimafs_scan_directory(minimafs_drive_t* drive, uint32_t block);
void minimafs_refresh_storage_desc(minimafs_drive_t* drive);
bool minimafs_write_storage_desc(minimafs_drive_t* drive);

uint32_t minimafs_tell(minimafs_file_handle_t* handle);
uint32_t minimafs_size(minimafs_file_handle_t* handle);
bool minimafs_eof(minimafs_file_handle_t* handle);
int32_t findinfile(const char* needle, const char* path);
bool minimafs_read_line(minimafs_file_handle_t* handle, char* buffer, size_t buffer_size);
bool getvalfromsplit(const char* str, const char* delimiter, int index, char* output, size_t output_size);
int32_t minimafs_parse_int(const char* str);
bool minimafs_get_config_value(const char* key, const char* path, char* output, size_t output_size);

#endif // MINIMAFS_H
