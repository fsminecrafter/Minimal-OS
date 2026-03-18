#include "x86_64/minimafs.h"
#include "serial.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "time.h"
#include "x86_64/ahci.h"

//disk drive wrapper like a whooopper

typedef struct {
    ahci_drive_t* ahci_drive;
    uint32_t sector_size;
} minimafs_disk_device_t;

// ===========================================
// GLOBAL STATE
// ===========================================

static minimafs_drive_t g_drives[MINIMAFS_MAX_DRIVES];
static bool g_initialized = false;

// DMA bounce buffers (per drive)
#define MINIMAFS_DMA_BOUNCE_BLOCKS 16  // 16 * 4KB = 64KB
static void* g_dma_bounce[MINIMAFS_MAX_DRIVES];
static bool g_self_test_done[MINIMAFS_MAX_DRIVES];

#define MINIMAFS_ENABLE_SELF_TEST 1
#define MINIMAFS_SELF_TEST_MAX_SECTOR 4096

// ===========================================
// INTERNAL HELPERS
// ===========================================

static minimafs_drive_t* get_drive(uint8_t drive_number) {
    if (drive_number == 0 || drive_number > MINIMAFS_MAX_DRIVES) {
        return NULL;
    }
    return &g_drives[drive_number - 1];
}

static bool minimafs_read_blocks(minimafs_drive_t* drive, uint32_t block_num,
                                 uint32_t count, void* buffer);
static bool minimafs_write_blocks(minimafs_drive_t* drive, uint32_t block_num,
                                  uint32_t count, const void* buffer);
static uint32_t block_alloc_run(uint8_t drive_num, uint32_t count);
static void block_free_run(uint8_t drive_num, uint32_t block_num, uint32_t count);
bool minimafs_read_folder_desc(minimafs_drive_t* drive, const char* path,
                               minimafs_folder_desc_t* desc);
bool minimafs_write_folder_desc(minimafs_drive_t* drive, const minimafs_folder_desc_t* desc);
static bool minimafs_write_storage_desc(minimafs_drive_t* drive, const minimafs_storage_desc_t* desc);
static bool minimafs_read_storage_desc(minimafs_drive_t* drive, minimafs_storage_desc_t* desc);
static void minimafs_refresh_storage_desc(minimafs_drive_t* drive);

static bool ensure_dma_bounce(uint8_t drive_number) {
    if (drive_number == 0 || drive_number > MINIMAFS_MAX_DRIVES) {
        return false;
    }
    uint8_t idx = drive_number - 1;
    if (g_dma_bounce[idx]) {
        return true;
    }
    
    // Allocate contiguous 4KB pages from PMM (identity-mapped, DMA-safe)
    void* base = NULL;
    for (uint32_t i = 0; i < MINIMAFS_DMA_BOUNCE_BLOCKS; i++) {
        void* page = alloc_page_zeroed();
        if (!page) {
            serial_write_str("MinimaFS: Failed to allocate DMA page\n");
            return false;
        }
        if (i == 0) {
            base = page;
        } else {
            uintptr_t expected = (uintptr_t)base + (i * MINIMAFS_BLOCK_SIZE);
            if ((uintptr_t)page != expected) {
                serial_write_str("MinimaFS: DMA pages not contiguous\n");
                return false;
            }
        }
    }
    
    g_dma_bounce[idx] = base;
    return true;
}

static bool minimafs_self_test_drive(minimafs_drive_t* drive) {
    if (!drive || !drive->device_handle) return false;

    minimafs_disk_device_t* disk = (minimafs_disk_device_t*)drive->device_handle;
    if (!disk->ahci_drive || disk->sector_size == 0) return false;

    if (disk->sector_size > MINIMAFS_SELF_TEST_MAX_SECTOR) {
        serial_write_str("MinimaFS: Self-test skipped (sector too large)\n");
        return true;
    }

    uint64_t sectors = disk->ahci_drive->sectors;
    if (sectors < 2) {
        serial_write_str("MinimaFS: Self-test skipped (unknown size)\n");
        return true;
    }

    uint64_t lba = sectors - 1;
    uint8_t* page = (uint8_t*)alloc_page_zeroed();
    if (!page) return false;

    uint8_t original[MINIMAFS_SELF_TEST_MAX_SECTOR];

    if (!ahci_read(disk->ahci_drive, lba, 1, page)) {
        serial_write_str("MinimaFS: Self-test read failed\n");
        return false;
    }

    memcpy(original, page, disk->sector_size);

    for (uint32_t i = 0; i < disk->sector_size; i++) {
        page[i] = (uint8_t)(0xA5u ^ (i & 0xFFu));
    }

    bool ok = true;
    if (!ahci_write(disk->ahci_drive, lba, 1, page)) {
        serial_write_str("MinimaFS: Self-test write failed\n");
        ok = false;
    }

    memset(page, 0, disk->sector_size);

    if (!ahci_read(disk->ahci_drive, lba, 1, page)) {
        serial_write_str("MinimaFS: Self-test readback failed\n");
        ok = false;
    } else {
        for (uint32_t i = 0; i < disk->sector_size; i++) {
            uint8_t expected = (uint8_t)(0xA5u ^ (i & 0xFFu));
            if (page[i] != expected) {
                ok = false;
                serial_write_str("MinimaFS: Self-test verify mismatch\n");
                break;
            }
        }
    }

    // Restore original sector
    memcpy(page, original, disk->sector_size);
    if (!ahci_write(disk->ahci_drive, lba, 1, page)) {
        serial_write_str("MinimaFS: Self-test restore failed\n");
        ok = false;
    }

    return ok;
}

// ===========================================
// INITIALIZATION
// ===========================================

void minimafs_init(void) {
    serial_write_str("MinimaFS: Initializing...\n");
    
    memset(g_drives, 0, sizeof(g_drives));
    memset(g_dma_bounce, 0, sizeof(g_dma_bounce));
    memset(g_self_test_done, 0, sizeof(g_self_test_done));
    
    for (int i = 0; i < MINIMAFS_MAX_DRIVES; i++) {
        g_drives[i].mounted = false;
        g_drives[i].drive_number = i + 1;
    }
    
    g_initialized = true;
    serial_write_str("MinimaFS: Ready\n");
}

// ===========================================
// DATE/TIME HELPERS
// ===========================================

void minimafs_get_datetime(char* buffer, size_t size) {
    // Format: "18Mar2026"
    datetime_t dt;
    dt = time_get_datetime();
    const char* mon = time_get_month_name_short(dt.month);
    if (!mon || mon[0] == '\0') mon = "???";
    
    if (size < 10) return;
    
    buffer[0] = '0' + (dt.day / 10);
    buffer[1] = '0' + (dt.day % 10);
    buffer[2] = mon[0];
    buffer[3] = mon[1];
    buffer[4] = mon[2];
    buffer[5] = '0' + ((dt.year / 1000) % 10);
    buffer[6] = '0' + ((dt.year / 100) % 10);
    buffer[7] = '0' + ((dt.year / 10) % 10);
    buffer[8] = '0' + (dt.year % 10);
    buffer[9] = '\0';
}

// ===========================================
// PATH PARSING
// ===========================================

bool minimafs_parse_path(const char* path, uint8_t* drive_number, char* local_path) {
    if (!path || !drive_number || !local_path) {
        return false;
    }
    
    // Format: "1:/path/to/file" or "mydrive:/path"
    const char* colon = strchr(path, ':');
    if (!colon) {
        serial_write_str("MinimaFS: Invalid path format (missing ':'): ");
        serial_write_str(path);
        serial_write_str("\n");
        return false;
    }
    
    // Extract drive part
    size_t drive_len = colon - path;
    if (drive_len == 0 || drive_len > 63) {
        return false;
    }
    
    char drive_str[64];
    memcpy(drive_str, path, drive_len);
    drive_str[drive_len] = '\0';
    
    // Check if numeric (1-99) or name
    if (drive_str[0] >= '0' && drive_str[0] <= '9') {
        // Numeric drive
        *drive_number = 0;
        for (size_t i = 0; i < drive_len; i++) {
            if (drive_str[i] < '0' || drive_str[i] > '9') {
                return false;
            }
            *drive_number = (*drive_number * 10) + (drive_str[i] - '0');
        }
        
        if (*drive_number == 0 || *drive_number > MINIMAFS_MAX_DRIVES) {
            return false;
        }
    } else {
        // Named drive - lookup by name
        bool found = false;
        for (int i = 0; i < MINIMAFS_MAX_DRIVES; i++) {
            if (g_drives[i].mounted && strcmp(g_drives[i].drive_name, drive_str) == 0) {
                *drive_number = g_drives[i].drive_number;
                found = true;
                break;
            }
        }
        
        if (!found) {
            serial_write_str("MinimaFS: Drive not found: ");
            serial_write_str(drive_str);
            serial_write_str("\n");
            return false;
        }
    }
    
    // Copy local path
    strcpy(local_path, colon + 1);
    
    return true;
}

// ===========================================
// FILE FORMAT PARSING
// ===========================================

static bool parse_tag(const char* line, const char* tag, char* value, size_t max_len) {
    // Parse line like "@FILENAME:'hello.txt'@"
    char tag_start[128];
    snprintf(tag_start, sizeof(tag_start), "@%s:", tag);
    
    const char* start = strstr(line, tag_start);
    if (!start) return false;
    
    start += strlen(tag_start);
    
    // Check for quote
    if (*start == '\'') {
        start++;
        const char* end = strchr(start, '\'');
        if (!end) return false;
        
        size_t len = end - start;
        if (len >= max_len) len = max_len - 1;
        
        memcpy(value, start, len);
        value[len] = '\0';
        return true;
    } else {
        // No quotes - read until @
        const char* end = strchr(start, '@');
        if (!end) return false;
        
        size_t len = end - start;
        if (len >= max_len) len = max_len - 1;
        
        memcpy(value, start, len);
        value[len] = '\0';
        
        // Trim trailing whitespace
        while (len > 0 && (value[len-1] == ' ' || value[len-1] == '\t')) {
            value[--len] = '\0';
        }
        
        return true;
    }
}

static bool parse_tag_bool(const char* line, const char* tag) {
    char value[16];
    if (!parse_tag(line, tag, value, sizeof(value))) {
        return false;
    }
    
    return strcmp(value, "True") == 0 || strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

static uint64_t parse_tag_uint64(const char* line, const char* tag) {
    char value[32];
    if (!parse_tag(line, tag, value, sizeof(value))) {
        return 0;
    }
    
    // Parse hex if starts with 0x
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        uint64_t result = 0;
        for (int i = 2; value[i]; i++) {
            result <<= 4;
            if (value[i] >= '0' && value[i] <= '9') {
                result += value[i] - '0';
            } else if (value[i] >= 'a' && value[i] <= 'f') {
                result += value[i] - 'a' + 10;
            } else if (value[i] >= 'A' && value[i] <= 'F') {
                result += value[i] - 'A' + 10;
            }
        }
        return result;
    }
    
    // Parse decimal
    uint64_t result = 0;
    for (int i = 0; value[i]; i++) {
        if (value[i] >= '0' && value[i] <= '9') {
            result = result * 10 + (value[i] - '0');
        }
    }
    return result;
}

bool minimafs_parse_file_header(const char* data, minimafs_file_metadata_t* metadata) {
    if (!data || !metadata) return false;
    
    // Check for @HEADER@
    if (strncmp(data, "@HEADER@", 8) != 0) {
        serial_write_str("MinimaFS: Missing @HEADER@\n");
        return false;
    }
    
    memset(metadata, 0, sizeof(minimafs_file_metadata_t));
    
    // Parse line by line
    const char* line = data;
    while (*line) {
        // Find line end
        const char* line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);
        
        // Check for end of header
        if (strncmp(line, "@DATA@", 6) == 0) {
            break;
        }
        
        // Parse tags
        char temp[512];
        
        if (strstr(line, "@FILETYPE:")) {
            parse_tag(line, "FILETYPE", metadata->filetype, sizeof(metadata->filetype));
        }
        else if (strstr(line, "@FILEFORMAT:")) {
            parse_tag(line, "FILEFORMAT", metadata->fileformat, sizeof(metadata->fileformat));
        }
        else if (strstr(line, "@FILELEN:")) {
            metadata->file_length = parse_tag_uint64(line, "FILELEN");
        }
        else if (strstr(line, "@DATALEN:")) {
            metadata->data_length = parse_tag_uint64(line, "DATALEN");
        }
        else if (strstr(line, "@FILENAME:")) {
            parse_tag(line, "FILENAME", metadata->filename, sizeof(metadata->filename));
        }
        else if (strstr(line, "@CREATEDDATE:")) {
            parse_tag(line, "CREATEDDATE", metadata->created_date, sizeof(metadata->created_date));
        }
        else if (strstr(line, "@LASTCHANGED:")) {
            parse_tag(line, "LASTCHANGED", metadata->last_changed, sizeof(metadata->last_changed));
        }
        else if (strstr(line, "@PARENTFOLDER:")) {
            parse_tag(line, "PARENTFOLDER", metadata->parent_folder, sizeof(metadata->parent_folder));
        }
        else if (strstr(line, "@RUNNABLE:")) {
            metadata->runnable = parse_tag_bool(line, "RUNNABLE");
        }
        else if (strstr(line, "@ENTRYPOINT:")) {
            metadata->entrypoint = parse_tag_uint64(line, "ENTRYPOINT");
        }
        else if (strstr(line, "@RUNWITH:")) {
            parse_tag(line, "RUNWITH", metadata->run_with, sizeof(metadata->run_with));
        }
        else if (strstr(line, "@HIDDEN:")) {
            metadata->hidden = parse_tag_bool(line, "HIDDEN");
        }
        
        // Next line
        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }
    
    return true;
}

// MinimaFS Part 2 - File Operations

// ===========================================
// FILE FORMAT GENERATION
// ===========================================

static char* minimafs_generate_file_header(const minimafs_file_metadata_t* metadata, uint32_t* header_size) {
    // Allocate buffer for header (max 2KB)
    char* header = (char*)alloc(2048);
    if (!header) return NULL;
    
    char* ptr = header;
    
    // Write header
    ptr += sprintf(ptr, "@HEADER@\n");
    ptr += sprintf(ptr, "@FILETYPE:%s@\n", metadata->filetype);
    ptr += sprintf(ptr, "@FILEFORMAT:%s@\n", metadata->fileformat);
    ptr += sprintf(ptr, "@FILELEN:%u@\n", metadata->file_length);
    ptr += sprintf(ptr, "@DATALEN:%u@\n", metadata->data_length);
    ptr += sprintf(ptr, "@FILENAME:'%s'@\n", metadata->filename);
    ptr += sprintf(ptr, "@CREATEDDATE:'%s'@\n", metadata->created_date);
    ptr += sprintf(ptr, "@LASTCHANGED:'%s'@\n", metadata->last_changed);
    ptr += sprintf(ptr, "@PARENTFOLDER:'%s'@\n", metadata->parent_folder);
    
    if (metadata->runnable) {
        ptr += sprintf(ptr, "@RUNNABLE:True@\n");
        if (metadata->entrypoint != 0) {
            char hexbuf[32];
            hex_to_str(metadata->entrypoint, hexbuf);
            ptr += sprintf(ptr, "@ENTRYPOINT:0x%s@\n", hexbuf);
        }
        if (metadata->run_with[0] != '\0') {
            ptr += sprintf(ptr, "@RUNWITH:%s@\n", metadata->run_with);
        }
    } else {
        ptr += sprintf(ptr, "@RUNNABLE:False@\n");
    }
    
    ptr += sprintf(ptr, "@HIDDEN:%s@\n", metadata->hidden ? "True" : "False");
    ptr += sprintf(ptr, "@DATA@\n");
    
    *header_size = ptr - header;
    return header;
}

static bool minimafs_write_file_to_disk(minimafs_drive_t* drive, const char* local_path,
                                        minimafs_file_metadata_t* metadata,
                                        const void* data, uint32_t data_size) {
    // Generate header
    if (!drive || !metadata) return false;
    
    metadata->data_length = data_size;
    metadata->file_length = 0;
    
    uint32_t header_size = 0;
    uint32_t footer_size = 5;  // "@END\n"
    for (int i = 0; i < 2; i++) {
        metadata->file_length = header_size + data_size + footer_size;
        char* tmp = minimafs_generate_file_header(metadata, &header_size);
        if (!tmp) return false;
        free_mem(tmp);
    }
    metadata->file_length = header_size + data_size + footer_size;
    
    char* header = minimafs_generate_file_header(metadata, &header_size);
    if (!header) return false;
    
    uint32_t total_size = header_size + data_size + footer_size;
    uint32_t aligned_size = ((total_size + MINIMAFS_BLOCK_SIZE - 1) / MINIMAFS_BLOCK_SIZE) * MINIMAFS_BLOCK_SIZE;
    uint32_t block_count = aligned_size / MINIMAFS_BLOCK_SIZE;
    
    uint32_t start_block = metadata->block_offset;
    uint32_t old_count = metadata->block_count;
    uint32_t old_offset = metadata->block_offset;
    
    if (start_block != 0 && old_count >= block_count) {
        if (old_count > block_count) {
            block_free_run(drive->drive_number, old_offset + block_count, old_count - block_count);
        }
    } else {
        start_block = block_alloc_run(drive->drive_number, block_count);
        if (start_block == 0xFFFFFFFF) {
            free_mem(header);
            return false;
        }
        if (old_count > 0) {
            block_free_run(drive->drive_number, old_offset, old_count);
        }
        metadata->block_offset = start_block;
    }
    
    metadata->block_count = block_count;
    
    char* buffer = (char*)alloc(aligned_size);
    if (!buffer) {
        free_mem(header);
        block_free_run(drive->drive_number, start_block, block_count);
        return false;
    }
    
    memset(buffer, 0, aligned_size);
    memcpy(buffer, header, header_size);
    free_mem(header);
    
    if (data && data_size > 0) {
        memcpy(buffer + header_size, data, data_size);
    }
    memcpy(buffer + header_size + data_size, "@END\n", 5);
    
    bool ok = minimafs_write_blocks(drive, start_block, block_count, buffer);
    free_mem(buffer);
    
    if (!ok) {
        block_free_run(drive->drive_number, start_block, block_count);
        return false;
    }
    
    minimafs_refresh_storage_desc(drive);
    (void)local_path;
    return true;
}

// ===========================================
// FILE OPERATIONS
// ===========================================

static void minimafs_split_local_path(const char* local_path, char* parent, char* name) {
    const char* last_slash = strrchr(local_path, '/');
    if (last_slash) {
        strcpy(name, last_slash + 1);
        size_t parent_len = last_slash - local_path;
        memcpy(parent, local_path, parent_len);
        parent[parent_len] = '\0';
    } else {
        strcpy(name, local_path);
        parent[0] = '\0';
    }
}

static bool minimafs_find_entry_in_folder(minimafs_folder_desc_t* desc, const char* name, uint32_t* index) {
    if (!desc || !name) return false;
    for (uint32_t i = 0; i < desc->entry_count; i++) {
        if (strcmp(desc->entries[i].name, name) == 0) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

bool minimafs_create_file(const char* path, const char* filetype, const char* fileformat) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return false;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        serial_write_str("MinimaFS: Drive not mounted\n");
        return false;
    }
    
    // Extract filename and parent folder
    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);
    if (filename[0] == '\0') {
        return false;
    }
    
    // Read parent folder.desc
    minimafs_folder_desc_t parent_desc;
    if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
        return false;
    }
    
    uint32_t existing_index = 0;
    bool exists = minimafs_find_entry_in_folder(&parent_desc, filename, &existing_index);
    if (exists && parent_desc.entries[existing_index].type == MINIMAFS_TYPE_DIR) {
        return false;
    }
    
    // Create metadata
    minimafs_file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    
    strcpy(metadata.filename, filename);
    strcpy(metadata.filetype, filetype);
    strcpy(metadata.fileformat, fileformat);
    strcpy(metadata.parent_folder, parent);
    
    minimafs_get_datetime(metadata.created_date, sizeof(metadata.created_date));
    minimafs_get_datetime(metadata.last_changed, sizeof(metadata.last_changed));
    
    metadata.file_length = 0;
    metadata.data_length = 0;
    metadata.runnable = false;
    metadata.hidden = false;
    
    // Write empty file
    if (!minimafs_write_file_to_disk(drive, local_path, &metadata, NULL, 0)) {
        return false;
    }
    
    if (exists) {
        minimafs_dir_entry_t* entry = &parent_desc.entries[existing_index];
        entry->type = MINIMAFS_TYPE_FILE;
        entry->block_offset = metadata.block_offset;
        entry->block_count = metadata.block_count;
        entry->hidden = metadata.hidden;
    } else {
        if (parent_desc.entry_count >= 256) {
            return false;
        }
        minimafs_dir_entry_t* entry = &parent_desc.entries[parent_desc.entry_count++];
        strcpy(entry->name, filename);
        entry->type = MINIMAFS_TYPE_FILE;
        entry->block_offset = metadata.block_offset;
        entry->block_count = metadata.block_count;
        entry->hidden = metadata.hidden;
    }
    
    return minimafs_write_folder_desc(drive, &parent_desc);
}

minimafs_file_handle_t* minimafs_open(const char* path, bool read_only) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return NULL;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        serial_write_str("MinimaFS: Drive not mounted\n");
        return NULL;
    }
    
    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);
    
    minimafs_folder_desc_t parent_desc;
    if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
        return NULL;
    }
    
    uint32_t entry_index = 0;
    if (!minimafs_find_entry_in_folder(&parent_desc, filename, &entry_index)) {
        if (read_only) {
            return NULL;
        }
        if (!minimafs_create_file(path, "binary", "bin")) {
            return NULL;
        }
        if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
            return NULL;
        }
        if (!minimafs_find_entry_in_folder(&parent_desc, filename, &entry_index)) {
            return NULL;
        }
    }
    
    minimafs_dir_entry_t entry = parent_desc.entries[entry_index];
    if (entry.type == MINIMAFS_TYPE_DIR) {
        return NULL;
    }
    
    // Allocate file handle
    minimafs_file_handle_t* handle = (minimafs_file_handle_t*)alloc(sizeof(minimafs_file_handle_t));
    if (!handle) return NULL;
    
    memset(handle, 0, sizeof(minimafs_file_handle_t));
    
    handle->open = true;
    handle->drive_number = drive_num;
    strcpy(handle->path, path);
    handle->read_only = read_only;
    handle->position = 0;
    handle->modified = false;
    
    uint32_t total_size = entry.block_count * MINIMAFS_BLOCK_SIZE;
    if (total_size == 0) {
        memset(&handle->metadata, 0, sizeof(handle->metadata));
        handle->data = NULL;
        handle->data_size = 0;
    } else {
        uint8_t* raw = (uint8_t*)alloc(total_size);
        if (!raw) {
            free_mem(handle);
            return NULL;
        }
        
        if (!minimafs_read_blocks(drive, entry.block_offset, entry.block_count, raw)) {
            free_mem(raw);
            free_mem(handle);
            return NULL;
        }
        
        minimafs_file_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        minimafs_parse_file_header((const char*)raw, &meta);
        meta.block_offset = entry.block_offset;
        meta.block_count = entry.block_count;
        
        const char* data_marker = strstr((const char*)raw, "@DATA@\n");
        uint32_t data_offset = data_marker ? (uint32_t)(data_marker - (const char*)raw) + 7 : 0;
        uint32_t data_length = meta.data_length;
        if (data_length == 0 && meta.file_length > data_offset + 5) {
            data_length = meta.file_length - data_offset - 5;
        }
        if (data_offset + data_length > total_size) {
            if (total_size > data_offset) {
                data_length = total_size - data_offset;
            } else {
                data_length = 0;
            }
        }
        
        handle->metadata = meta;
        handle->data = NULL;
        handle->data_size = data_length;
        
        if (data_length > 0) {
            handle->data = (uint8_t*)alloc(data_length);
            if (!handle->data) {
                free_mem(raw);
                free_mem(handle);
                return NULL;
            }
            memcpy(handle->data, raw + data_offset, data_length);
        }
        
        free_mem(raw);
    }
    
    serial_write_str("MinimaFS: Opened ");
    serial_write_str(path);
    serial_write_str(read_only ? " (read-only)\n" : " (read-write)\n");
    
    return handle;
}

void minimafs_close(minimafs_file_handle_t* handle) {
    if (!handle) return;
    
    if (handle->modified && !handle->read_only) {
        // Write changes back to disk
        serial_write_str("MinimaFS: Writing changes to ");
        serial_write_str(handle->path);
        serial_write_str("\n");
        
        minimafs_drive_t* drive = get_drive(handle->drive_number);
        if (drive && drive->mounted) {
            char local_path[MINIMAFS_MAX_PATH];
            uint8_t dn;
            if (minimafs_parse_path(handle->path, &dn, local_path)) {
                if (minimafs_write_file_to_disk(drive, local_path, &handle->metadata,
                                               handle->data, handle->data_size)) {
                    char filename[MINIMAFS_MAX_FILENAME];
                    char parent[MINIMAFS_MAX_PATH];
                    minimafs_split_local_path(local_path, parent, filename);
                    
                    minimafs_folder_desc_t parent_desc;
                    if (minimafs_read_folder_desc(drive, parent, &parent_desc)) {
                        uint32_t idx = 0;
                        if (minimafs_find_entry_in_folder(&parent_desc, filename, &idx)) {
                            minimafs_dir_entry_t* entry = &parent_desc.entries[idx];
                            entry->block_offset = handle->metadata.block_offset;
                            entry->block_count = handle->metadata.block_count;
                            entry->hidden = handle->metadata.hidden;
                            minimafs_write_folder_desc(drive, &parent_desc);
                        }
                    }
                }
            }
        }
    }
    
    if (handle->data) {
        free_mem(handle->data);
    }
    
    handle->open = false;
    free_mem(handle);
}

uint32_t minimafs_read(minimafs_file_handle_t* handle, void* buffer, uint32_t size) {
    if (!handle || !handle->open || !buffer) return 0;
    
    // Check bounds
    if (handle->position >= handle->data_size) {
        return 0;  // EOF
    }
    
    // Calculate how much to read
    uint32_t available = handle->data_size - handle->position;
    uint32_t to_read = (size < available) ? size : available;
    
    // Copy data
    memcpy(buffer, handle->data + handle->position, to_read);
    handle->position += to_read;
    
    return to_read;
}

uint32_t minimafs_write(minimafs_file_handle_t* handle, const void* buffer, uint32_t size) {
    if (!handle || !handle->open || handle->read_only || !buffer) return 0;
    
    // Check if we need to expand buffer
    uint32_t needed_size = handle->position + size;
    
    if (needed_size > handle->data_size) {
        // Reallocate
        uint8_t* new_data = (uint8_t*)alloc(needed_size);
        if (!new_data) return 0;
        
        if (handle->data) {
            memcpy(new_data, handle->data, handle->data_size);
            free_mem(handle->data);
        }
        
        handle->data = new_data;
        handle->data_size = needed_size;
    }
    
    // Write data
    memcpy(handle->data + handle->position, buffer, size);
    handle->position += size;
    handle->modified = true;
    
    // Update metadata
    minimafs_get_datetime(handle->metadata.last_changed, sizeof(handle->metadata.last_changed));
    handle->metadata.data_length = handle->data_size;
    
    return size;
}

void minimafs_seek(minimafs_file_handle_t* handle, uint32_t position) {
    if (!handle || !handle->open) return;
    handle->position = position;
}

bool minimafs_delete_file(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return false;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        return false;
    }
    
    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);
    
    minimafs_folder_desc_t parent_desc;
    if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
        return false;
    }
    
    uint32_t idx = 0;
    if (!minimafs_find_entry_in_folder(&parent_desc, filename, &idx)) {
        return false;
    }
    
    minimafs_dir_entry_t entry = parent_desc.entries[idx];
    if (entry.type == MINIMAFS_TYPE_DIR) {
        return false;
    }
    
    if (entry.block_count > 0) {
        block_free_run(drive_num, entry.block_offset, entry.block_count);
    }
    
    // Remove entry by shifting
    for (uint32_t i = idx; i + 1 < parent_desc.entry_count; i++) {
        parent_desc.entries[i] = parent_desc.entries[i + 1];
    }
    parent_desc.entry_count--;
    
    minimafs_refresh_storage_desc(drive);
    return minimafs_write_folder_desc(drive, &parent_desc);
}

static bool minimafs_read_blocks(minimafs_drive_t* drive, uint32_t block_num, 
                                 uint32_t count, void* buffer) {
    if (!drive || !drive->device_handle) return false;
    
    minimafs_disk_device_t* disk = (minimafs_disk_device_t*)drive->device_handle;
    if (!disk->ahci_drive || disk->sector_size == 0) return false;
    if (!ensure_dma_bounce(drive->drive_number)) return false;
    if (!buffer) return false;
    if (count == 0) return true;
    
    uint8_t* out = (uint8_t*)buffer;
    uint8_t* bounce = (uint8_t*)g_dma_bounce[drive->drive_number - 1];
    uint32_t remaining = count;
    uint32_t current_block = block_num;
    
    while (remaining > 0) {
        uint32_t chunk = (remaining > MINIMAFS_DMA_BOUNCE_BLOCKS) ? MINIMAFS_DMA_BOUNCE_BLOCKS : remaining;
        
        // Convert blocks to sectors (4KB blocks = 8 x 512-byte sectors)
        uint32_t sectors_per_block = MINIMAFS_BLOCK_SIZE / disk->sector_size;
        uint64_t start_sector = (uint64_t)current_block * sectors_per_block;
        uint32_t sector_count = chunk * sectors_per_block;
        
        if (!ahci_read(disk->ahci_drive, start_sector, sector_count, bounce)) {
            return false;
        }
        
        memcpy(out, bounce, chunk * MINIMAFS_BLOCK_SIZE);
        out += chunk * MINIMAFS_BLOCK_SIZE;
        current_block += chunk;
        remaining -= chunk;
    }
    
    return true;
}
 
static bool minimafs_write_blocks(minimafs_drive_t* drive, uint32_t block_num,
                                  uint32_t count, const void* buffer) {
    if (!drive || !drive->device_handle) return false;
    
    minimafs_disk_device_t* disk = (minimafs_disk_device_t*)drive->device_handle;
    if (!disk->ahci_drive || disk->sector_size == 0) return false;
    if (!ensure_dma_bounce(drive->drive_number)) return false;
    if (!buffer) return false;
    if (count == 0) return true;
    
    const uint8_t* in = (const uint8_t*)buffer;
    uint8_t* bounce = (uint8_t*)g_dma_bounce[drive->drive_number - 1];
    uint32_t remaining = count;
    uint32_t current_block = block_num;
    
    while (remaining > 0) {
        uint32_t chunk = (remaining > MINIMAFS_DMA_BOUNCE_BLOCKS) ? MINIMAFS_DMA_BOUNCE_BLOCKS : remaining;
        
        memcpy(bounce, in, chunk * MINIMAFS_BLOCK_SIZE);
        
        uint32_t sectors_per_block = MINIMAFS_BLOCK_SIZE / disk->sector_size;
        uint64_t start_sector = (uint64_t)current_block * sectors_per_block;
        uint32_t sector_count = chunk * sectors_per_block;
        
        if (!ahci_write(disk->ahci_drive, start_sector, sector_count, bounce)) {
            return false;
        }
        
        in += chunk * MINIMAFS_BLOCK_SIZE;
        current_block += chunk;
        remaining -= chunk;
    }
    
    return true;
}
 
// ===========================================
// BLOCK ALLOCATOR
// ===========================================
 
#define MINIMAFS_BITMAP_SIZE 8192  // 8KB bitmap = 65536 blocks = 256MB
 
typedef struct {
    uint8_t bitmap[MINIMAFS_BITMAP_SIZE];  // Bitmap of free/used blocks
    uint32_t total_blocks;
    uint32_t free_blocks;
} minimafs_block_alloc_t;
 
static minimafs_block_alloc_t g_block_alloc[MINIMAFS_MAX_DRIVES];
 
static void block_alloc_init(uint8_t drive_num, uint32_t total_blocks) {
    minimafs_block_alloc_t* alloc = &g_block_alloc[drive_num - 1];
    
    memset(alloc->bitmap, 0, MINIMAFS_BITMAP_SIZE);
    alloc->total_blocks = total_blocks;
    alloc->free_blocks = total_blocks;
    
    // Mark first block as used (storage.desc)
    alloc->bitmap[0] |= 1;
    alloc->free_blocks--;
}
 
static void block_alloc_mark_used(uint8_t drive_num, uint32_t block_num, uint32_t count) {
    minimafs_block_alloc_t* alloc = &g_block_alloc[drive_num - 1];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b = block_num + i;
        uint32_t byte = b / 8;
        uint8_t bit = b % 8;
        if (byte >= MINIMAFS_BITMAP_SIZE) return;
        if ((alloc->bitmap[byte] & (1 << bit)) == 0) {
            alloc->bitmap[byte] |= (1 << bit);
            if (alloc->free_blocks > 0) {
                alloc->free_blocks--;
            }
        }
    }
}

static uint32_t block_alloc_run(uint8_t drive_num, uint32_t count) {
    minimafs_block_alloc_t* alloc = &g_block_alloc[drive_num - 1];
    if (count == 0) return 0xFFFFFFFF;
    
    uint32_t max_blocks = MINIMAFS_BITMAP_SIZE * 8;
    uint32_t run_start = 0;
    uint32_t run_len = 0;
    
    for (uint32_t b = 0; b < max_blocks; b++) {
        uint32_t byte = b / 8;
        uint8_t bit = b % 8;
        bool used = (alloc->bitmap[byte] & (1 << bit)) != 0;
        
        if (!used) {
            if (run_len == 0) run_start = b;
            run_len++;
            if (run_len >= count) {
                block_alloc_mark_used(drive_num, run_start, count);
                return run_start;
            }
        } else {
            run_len = 0;
        }
    }
    
    return 0xFFFFFFFF;
}

static uint32_t block_alloc(uint8_t drive_num) {
    return block_alloc_run(drive_num, 1);
}
 
static void block_free_run(uint8_t drive_num, uint32_t block_num, uint32_t count) {
    minimafs_block_alloc_t* alloc = &g_block_alloc[drive_num - 1];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b = block_num + i;
        uint32_t byte = b / 8;
        uint8_t bit = b % 8;
        if (byte >= MINIMAFS_BITMAP_SIZE) return;
        if (alloc->bitmap[byte] & (1 << bit)) {
            alloc->bitmap[byte] &= ~(1 << bit);
            alloc->free_blocks++;
        }
    }
}

static void minimafs_refresh_storage_desc(minimafs_drive_t* drive) {
    if (!drive) return;
    minimafs_block_alloc_t* alloc = &g_block_alloc[drive->drive_number - 1];
    drive->storage_desc.free_blocks = alloc->free_blocks;
    drive->storage_desc.used_blocks = alloc->total_blocks - alloc->free_blocks;
    drive->storage_desc.used_size = (uint64_t)drive->storage_desc.used_blocks * MINIMAFS_BLOCK_SIZE;
    drive->storage_desc.free_size = drive->storage_desc.total_size - drive->storage_desc.used_size;
    minimafs_write_storage_desc(drive, &drive->storage_desc);
}
 
// ===========================================
// FOLDER.DESC OPERATIONS
// ===========================================
 
static uint32_t parse_uint32(const char* str) {
    uint32_t result = 0;
    if (!str) return 0;
    while (*str && (*str < '0' || *str > '9')) str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}

static bool minimafs_write_storage_desc(minimafs_drive_t* drive, const minimafs_storage_desc_t* desc) {
    if (!drive || !desc) return false;
    char buffer[MINIMAFS_BLOCK_SIZE];
    memset(buffer, 0, sizeof(buffer));
    
    char* ptr = buffer;
    char numbuf[32];
    
    ptr += sprintf(ptr, "@HEADER@\n");
    
    hex_to_str(desc->magic, numbuf);
    ptr += sprintf(ptr, "@MAGIC:0x%s@\n", numbuf);
    
    uint_to_str(desc->drive_number, numbuf);
    ptr += sprintf(ptr, "@DRIVE_NUMBER:%s@\n", numbuf);
    
    ptr += sprintf(ptr, "@DRIVE_NAME:'%s'@\n", desc->drive_name);
    ptr += sprintf(ptr, "@PASSWORD:'%s'@\n", desc->password);
    ptr += sprintf(ptr, "@PASSWORDPROTECTED:%s@\n", desc->password_protected ? "True" : "False");
    
    uint_to_str(desc->total_size, numbuf);
    ptr += sprintf(ptr, "@TOTAL_SIZE:%s@\n", numbuf);
    uint_to_str(desc->used_size, numbuf);
    ptr += sprintf(ptr, "@USED_SIZE:%s@\n", numbuf);
    uint_to_str(desc->free_size, numbuf);
    ptr += sprintf(ptr, "@FREE_SIZE:%s@\n", numbuf);
    
    uint_to_str(desc->total_blocks, numbuf);
    ptr += sprintf(ptr, "@TOTAL_BLOCKS:%s@\n", numbuf);
    uint_to_str(desc->used_blocks, numbuf);
    ptr += sprintf(ptr, "@USED_BLOCKS:%s@\n", numbuf);
    uint_to_str(desc->free_blocks, numbuf);
    ptr += sprintf(ptr, "@FREE_BLOCKS:%s@\n", numbuf);
    uint_to_str(desc->root_block, numbuf);
    ptr += sprintf(ptr, "@ROOT_BLOCK:%s@\n", numbuf);
    
    ptr += sprintf(ptr, "@FILESYSTEM_LABEL:'%s'@\n", desc->filesystem_label);
    ptr += sprintf(ptr, "@CREATEDDATE:'%s'@\n", desc->created_date);
    ptr += sprintf(ptr, "@LASTMOUNTED:'%s'@\n", desc->last_mounted);
    ptr += sprintf(ptr, "@END\n");
    
    return minimafs_write_blocks(drive, 0, 1, buffer);
}

static bool minimafs_read_storage_desc(minimafs_drive_t* drive, minimafs_storage_desc_t* desc) {
    if (!drive || !desc) return false;
    
    char buffer[MINIMAFS_BLOCK_SIZE + 1];
    if (!minimafs_read_blocks(drive, 0, 1, buffer)) {
        serial_write_str("MinimaFS: Read storage.desc failed\n");
        return false;
    }
    buffer[MINIMAFS_BLOCK_SIZE] = '\0';
    
    memset(desc, 0, sizeof(minimafs_storage_desc_t));
    desc->magic = (uint32_t)parse_tag_uint64(buffer, "MAGIC");
    desc->drive_number = (uint8_t)parse_tag_uint64(buffer, "DRIVE_NUMBER");
    parse_tag(buffer, "DRIVE_NAME", desc->drive_name, sizeof(desc->drive_name));
    parse_tag(buffer, "PASSWORD", desc->password, sizeof(desc->password));
    desc->password_protected = parse_tag_bool(buffer, "PASSWORDPROTECTED");
    desc->total_size = parse_tag_uint64(buffer, "TOTAL_SIZE");
    desc->used_size = parse_tag_uint64(buffer, "USED_SIZE");
    desc->free_size = parse_tag_uint64(buffer, "FREE_SIZE");
    desc->total_blocks = (uint32_t)parse_tag_uint64(buffer, "TOTAL_BLOCKS");
    desc->used_blocks = (uint32_t)parse_tag_uint64(buffer, "USED_BLOCKS");
    desc->free_blocks = (uint32_t)parse_tag_uint64(buffer, "FREE_BLOCKS");
    desc->root_block = (uint32_t)parse_tag_uint64(buffer, "ROOT_BLOCK");
    parse_tag(buffer, "FILESYSTEM_LABEL", desc->filesystem_label, sizeof(desc->filesystem_label));
    parse_tag(buffer, "CREATEDDATE", desc->created_date, sizeof(desc->created_date));
    parse_tag(buffer, "LASTMOUNTED", desc->last_mounted, sizeof(desc->last_mounted));
    
    if (desc->magic != MINIMAFS_MAGIC) {
        serial_write_str("MinimaFS: storage.desc magic mismatch: 0x");
        serial_write_hex(desc->magic);
        serial_write_str("\n");
        serial_write_str("MinimaFS: storage.desc head: ");
        serial_write_str(buffer);
        serial_write_str("\n");
        return false;
    }
    
    return true;
}

static bool parse_folder_entry_line(const char* line, minimafs_dir_entry_t* entry) {
    if (!line || !entry) return false;
    if (strncmp(line, "ENTRY:", 6) != 0) return false;
    
    const char* p = line + 6;
    const char* comma = strchr(p, ',');
    if (!comma) return false;
    
    size_t name_len = comma - p;
    if (name_len >= MINIMAFS_MAX_FILENAME) name_len = MINIMAFS_MAX_FILENAME - 1;
    memcpy(entry->name, p, name_len);
    entry->name[name_len] = '\0';
    
    const char* type_start = comma + 1;
    const char* block_tag = strstr(type_start, ",BLOCK:");
    if (!block_tag) return false;
    
    size_t type_len = block_tag - type_start;
    if (type_len >= 8) type_len = 7;
    char type_buf[8];
    memcpy(type_buf, type_start, type_len);
    type_buf[type_len] = '\0';
    
    entry->type = (strcmp(type_buf, "DIR") == 0) ? MINIMAFS_TYPE_DIR : MINIMAFS_TYPE_FILE;
    
    entry->block_offset = parse_uint32(block_tag + 7);
    entry->block_count = 1;
    entry->hidden = false;
    
    const char* count_tag = strstr(block_tag, ",COUNT:");
    if (count_tag) {
        entry->block_count = parse_uint32(count_tag + 7);
    }
    
    const char* hidden_tag = strstr(block_tag, ",HIDDEN:");
    if (hidden_tag) {
        entry->hidden = parse_uint32(hidden_tag + 8) != 0;
    }
    
    return true;
}

static bool minimafs_read_folder_desc_block(minimafs_drive_t* drive, uint32_t block,
                                            minimafs_folder_desc_t* desc) {
    char buffer[MINIMAFS_BLOCK_SIZE + 1];
    if (!minimafs_read_blocks(drive, block, 1, buffer)) {
        return false;
    }
    buffer[MINIMAFS_BLOCK_SIZE] = '\0';
    
    memset(desc, 0, sizeof(minimafs_folder_desc_t));
    desc->block_offset = block;
    uint32_t parsed_count = 0;
    uint32_t expected_count = 0;
    
    const char* line = buffer;
    while (*line) {
        const char* line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);
        
        if (strncmp(line, "FOLDER:", 7) == 0) {
            size_t len = line_end - (line + 7);
            if (len >= MINIMAFS_MAX_PATH) len = MINIMAFS_MAX_PATH - 1;
            memcpy(desc->path, line + 7, len);
            desc->path[len] = '\0';
        } else if (strncmp(line, "ENTRIES:", 8) == 0) {
            expected_count = parse_uint32(line + 8);
            if (expected_count > 256) expected_count = 256;
        } else if (strncmp(line, "ENTRY:", 6) == 0) {
            if (parsed_count < 256) {
                if (parse_folder_entry_line(line, &desc->entries[parsed_count])) {
                    parsed_count++;
                }
            }
        }
        
        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }
    
    desc->entry_count = parsed_count;
    (void)expected_count;
    
    return true;
}

static bool minimafs_get_folder_block(minimafs_drive_t* drive, const char* path, uint32_t* out_block) {
    if (!drive || !path || !out_block) return false;
    
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        *out_block = drive->storage_desc.root_block;
        return true;
    }
    
    char temp[MINIMAFS_MAX_PATH];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    if (temp[0] == '/') {
        memmove(temp, temp + 1, strlen(temp));
    }
    
    uint32_t current_block = drive->storage_desc.root_block;
    char* token = strtok(temp, "/");
    while (token) {
        minimafs_folder_desc_t desc;
        if (!minimafs_read_folder_desc_block(drive, current_block, &desc)) {
            return false;
        }
        
        bool found = false;
        for (uint32_t i = 0; i < desc.entry_count; i++) {
            minimafs_dir_entry_t* entry = &desc.entries[i];
            if (entry->type == MINIMAFS_TYPE_DIR && strcmp(entry->name, token) == 0) {
                current_block = entry->block_offset;
                found = true;
                break;
            }
        }
        
        if (!found) return false;
        token = strtok(NULL, "/");
    }
    
    *out_block = current_block;
    return true;
}

static uint32_t minimafs_calc_file_block_count(minimafs_drive_t* drive, uint32_t block_offset) {
    char buffer[MINIMAFS_BLOCK_SIZE + 1];
    if (!minimafs_read_blocks(drive, block_offset, 1, buffer)) {
        return 1;
    }
    buffer[MINIMAFS_BLOCK_SIZE] = '\0';
    
    minimafs_file_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    minimafs_parse_file_header(buffer, &meta);
    
    uint32_t file_len = meta.file_length;
    if (file_len == 0) {
        const char* data_marker = strstr(buffer, "@DATA@\n");
        if (data_marker) {
            uint32_t header_size = (uint32_t)(data_marker - buffer) + 7;
            file_len = header_size + 5;
        } else {
            file_len = MINIMAFS_BLOCK_SIZE;
        }
    }
    
    uint32_t aligned = ((file_len + MINIMAFS_BLOCK_SIZE - 1) / MINIMAFS_BLOCK_SIZE) * MINIMAFS_BLOCK_SIZE;
    return aligned / MINIMAFS_BLOCK_SIZE;
}

static void minimafs_scan_directory(minimafs_drive_t* drive, uint32_t block) {
    minimafs_folder_desc_t desc;
    if (!minimafs_read_folder_desc_block(drive, block, &desc)) {
        return;
    }
    
    for (uint32_t i = 0; i < desc.entry_count; i++) {
        minimafs_dir_entry_t* entry = &desc.entries[i];
        uint32_t count = entry->block_count;
        if (count == 0) {
            count = (entry->type == MINIMAFS_TYPE_DIR) ? 1 : minimafs_calc_file_block_count(drive, entry->block_offset);
        }
        block_alloc_mark_used(drive->drive_number, entry->block_offset, count);
        if (entry->type == MINIMAFS_TYPE_DIR) {
            minimafs_scan_directory(drive, entry->block_offset);
        }
    }
}

bool minimafs_read_folder_desc(minimafs_drive_t* drive, const char* path,
                               minimafs_folder_desc_t* desc) {
    if (!drive || !desc || !path) return false;
    
    uint32_t block;
    if (!minimafs_get_folder_block(drive, path, &block)) {
        return false;
    }
    
    if (!minimafs_read_folder_desc_block(drive, block, desc)) {
        return false;
    }
    
    // Ensure path is set even if the on-disk value is empty
    if (desc->path[0] == '\0') {
        strncpy(desc->path, path, sizeof(desc->path) - 1);
    }
    
    return true;
}
 
bool minimafs_write_folder_desc(minimafs_drive_t* drive, const minimafs_folder_desc_t* desc) {
    if (!drive || !desc) return false;
    if (desc->block_offset == 0) return false;
    
    char buffer[MINIMAFS_BLOCK_SIZE];
    memset(buffer, 0, sizeof(buffer));
    char* ptr = buffer;
    
    ptr += sprintf(ptr, "@HEADER@\n");
    ptr += sprintf(ptr, "@FILETYPE:descriptor@\n");
    ptr += sprintf(ptr, "@FILEFORMAT:desc@\n");
    ptr += sprintf(ptr, "@FILENAME:'folder.desc'@\n");
    ptr += sprintf(ptr, "@PARENTFOLDER:'%s'@\n", desc->path);
    ptr += sprintf(ptr, "@HIDDEN:True@\n");
    ptr += sprintf(ptr, "@DATA@\n");
    ptr += sprintf(ptr, "FOLDER:%s\n", desc->path);
    ptr += sprintf(ptr, "ENTRIES:%u\n", desc->entry_count);
    
    for (uint32_t i = 0; i < desc->entry_count; i++) {
        const minimafs_dir_entry_t* entry = &desc->entries[i];
        if ((size_t)(ptr - buffer) > MINIMAFS_BLOCK_SIZE - 128) {
            return false;
        }
        ptr += sprintf(ptr, "ENTRY:%s,%s,BLOCK:%u,COUNT:%u,HIDDEN:%u\n",
                      entry->name,
                      entry->type == MINIMAFS_TYPE_DIR ? "DIR" : "FILE",
                      entry->block_offset,
                      entry->block_count,
                      entry->hidden ? 1 : 0);
    }
    
    ptr += sprintf(ptr, "@END\n");
    
    return minimafs_write_blocks(drive, desc->block_offset, 1, buffer);
}
 
// ===========================================
// DIRECTORY OPERATIONS
// ===========================================
 
bool minimafs_mkdir(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return false;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        return false;
    }
    
    if (local_path[0] == '\0' || (local_path[0] == '/' && local_path[1] == '\0')) {
        return true;  // Root always exists
    }
    
    serial_write_str("MinimaFS: Creating directory ");
    serial_write_str(path);
    serial_write_str("\n");
    
    // Extract parent path
    char parent[MINIMAFS_MAX_PATH];
    char dirname[MINIMAFS_MAX_FILENAME];
    
    const char* last_slash = strrchr(local_path, '/');
    if (last_slash) {
        size_t parent_len = last_slash - local_path;
        memcpy(parent, local_path, parent_len);
        parent[parent_len] = '\0';
        strcpy(dirname, last_slash + 1);
    } else {
        parent[0] = '\0';
        strcpy(dirname, local_path);
    }
    
    if (dirname[0] == '\0') {
        return false;
    }
    
    // Read parent folder.desc
    minimafs_folder_desc_t parent_desc;
    if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
        return false;
    }
    
    // Allocate block for new directory
    uint32_t dir_block = block_alloc(drive_num);
    if (dir_block == 0xFFFFFFFF) {
        serial_write_str("MinimaFS: No free blocks\n");
        return false;
    }
    
    // Add entry to parent
    if (parent_desc.entry_count >= 256) {
        block_free_run(drive_num, dir_block, 1);
        return false;
    }
    
    minimafs_dir_entry_t* entry = &parent_desc.entries[parent_desc.entry_count++];
    strcpy(entry->name, dirname);
    entry->type = MINIMAFS_TYPE_DIR;
    entry->block_offset = dir_block;
    entry->block_count = 1;
    entry->hidden = false;
    
    // Write updated parent folder.desc
    if (!minimafs_write_folder_desc(drive, &parent_desc)) {
        block_free_run(drive_num, dir_block, 1);
        return false;
    }
    
    // Create empty folder.desc for new directory
    minimafs_folder_desc_t new_desc;
    memset(&new_desc, 0, sizeof(new_desc));
    strcpy(new_desc.path, local_path);
    new_desc.block_offset = dir_block;
    new_desc.entry_count = 0;
    
    if (!minimafs_write_folder_desc(drive, &new_desc)) {
        block_free_run(drive_num, dir_block, 1);
        return false;
    }
    minimafs_refresh_storage_desc(drive);
    return true;
}
 
bool minimafs_rmdir(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return false;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        return false;
    }
    
    if (local_path[0] == '\0' || (local_path[0] == '/' && local_path[1] == '\0')) {
        return false;  // Can't remove root
    }
    
    // Read target folder.desc
    minimafs_folder_desc_t target_desc;
    if (!minimafs_read_folder_desc(drive, local_path, &target_desc)) {
        return false;
    }
    if (target_desc.entry_count > 0) {
        return false;
    }
    
    // Remove from parent
    char dirname[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, dirname);
    
    minimafs_folder_desc_t parent_desc;
    if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
        return false;
    }
    
    uint32_t idx = 0;
    if (!minimafs_find_entry_in_folder(&parent_desc, dirname, &idx)) {
        return false;
    }
    
    if (parent_desc.entries[idx].type != MINIMAFS_TYPE_DIR) {
        return false;
    }
    
    uint32_t block_to_free = parent_desc.entries[idx].block_offset;
    
    for (uint32_t i = idx; i + 1 < parent_desc.entry_count; i++) {
        parent_desc.entries[i] = parent_desc.entries[i + 1];
    }
    parent_desc.entry_count--;
    
    if (!minimafs_write_folder_desc(drive, &parent_desc)) {
        return false;
    }
    
    if (block_to_free) {
        block_free_run(drive_num, block_to_free, 1);
    }
    
    minimafs_refresh_storage_desc(drive);
    return true;
}
 
uint32_t minimafs_list_dir(const char* path, minimafs_dir_entry_t* entries, uint32_t max_entries) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return 0;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        return 0;
    }
    
    // Read folder.desc
    minimafs_folder_desc_t desc;
    if (!minimafs_read_folder_desc(drive, local_path, &desc)) {
        return 0;
    }
    
    // Copy entries (skip hidden)
    uint32_t count = 0;
    for (uint32_t i = 0; i < desc.entry_count && count < max_entries; i++) {
        if (!desc.entries[i].hidden) {
            entries[count++] = desc.entries[i];
        }
    }
    
    return count;
}
 
bool minimafs_exists(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return false;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        return false;
    }
    
    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);
    
    if (filename[0] == '\0') return true;
    
    minimafs_folder_desc_t parent_desc;
    if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
        return false;
    }
    
    return minimafs_find_entry_in_folder(&parent_desc, filename, NULL);
}
 
bool minimafs_is_dir(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    
    if (!minimafs_parse_path(path, &drive_num, local_path)) {
        return false;
    }
    
    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        return false;
    }
    
    if (local_path[0] == '\0' || (local_path[0] == '/' && local_path[1] == '\0')) {
        return true;
    }
    
    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);
    
    minimafs_folder_desc_t parent_desc;
    if (!minimafs_read_folder_desc(drive, parent, &parent_desc)) {
        return false;
    }
    
    uint32_t idx = 0;
    if (!minimafs_find_entry_in_folder(&parent_desc, filename, &idx)) {
        return false;
    }
    
    return parent_desc.entries[idx].type == MINIMAFS_TYPE_DIR;
}
 
// ===========================================
// MOUNT/UNMOUNT
// ===========================================
 
bool minimafs_mount(void* device_handle, uint8_t drive_number) {
    if (drive_number == 0 || drive_number > MINIMAFS_MAX_DRIVES) {
        return false;
    }
    if (!device_handle) {
        serial_write_str("MinimaFS: Invalid device handle\n");
        return false;
    }
    
    minimafs_drive_t* drive = get_drive(drive_number);
    if (drive->mounted) {
        serial_write_str("MinimaFS: Drive already mounted\n");
        return false;
    }
    
    serial_write_str("MinimaFS: Mounting drive ");
    serial_write_dec(drive_number);
    serial_write_str("\n");
    
    drive->device_handle = device_handle;

#if MINIMAFS_ENABLE_SELF_TEST
    if (!g_self_test_done[drive_number - 1]) {
        serial_write_str("MinimaFS: Running self-test...\n");
        bool ok = minimafs_self_test_drive(drive);
        g_self_test_done[drive_number - 1] = true;
        if (!ok) {
            serial_write_str("MinimaFS: Self-test failed\n");
            return false;
        }
        serial_write_str("MinimaFS: Self-test passed\n");
    }
#endif
    
    if (!minimafs_read_storage_desc(drive, &drive->storage_desc)) {
        serial_write_str("MinimaFS: Failed to parse storage.desc\n");
        return false;
    }
    if (drive->storage_desc.root_block == 0) {
        serial_write_str("MinimaFS: Invalid root block\n");
        return false;
    }
    if (drive->storage_desc.total_blocks > (MINIMAFS_BITMAP_SIZE * 8)) {
        serial_write_str("MinimaFS: Drive too large for bitmap\n");
        return false;
    }
    
    if (drive->storage_desc.drive_number == 0) {
        drive->storage_desc.drive_number = drive_number;
    }
    
    drive->drive_number = drive_number;
    strncpy(drive->drive_name, drive->storage_desc.drive_name, sizeof(drive->drive_name) - 1);
    drive->drive_name[sizeof(drive->drive_name) - 1] = '\0';
    if (drive->drive_name[0] == '\0') {
        sprintf(drive->drive_name, "%u", drive_number);
    }
    
    // Initialize block allocator from storage.desc
    block_alloc_init(drive_number, drive->storage_desc.total_blocks);
    block_alloc_mark_used(drive_number, 0, 1);
    block_alloc_mark_used(drive_number, drive->storage_desc.root_block, 1);
    
    minimafs_scan_directory(drive, drive->storage_desc.root_block);
    minimafs_refresh_storage_desc(drive);
    
    minimafs_get_datetime(drive->storage_desc.last_mounted, sizeof(drive->storage_desc.last_mounted));
    minimafs_write_storage_desc(drive, &drive->storage_desc);
    
    drive->mounted = true;
    
    serial_write_str("MinimaFS: Drive ");
    serial_write_dec(drive_number);
    serial_write_str(" mounted successfully\n");
    
    return true;
}
 
bool minimafs_unmount(uint8_t drive_number) {
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive || !drive->mounted) {
        return false;
    }
    
    serial_write_str("MinimaFS: Unmounting drive ");
    serial_write_dec(drive_number);
    serial_write_str("\n");
    
    minimafs_refresh_storage_desc(drive);
    minimafs_write_storage_desc(drive, &drive->storage_desc);
    
    drive->mounted = false;
    drive->device_handle = NULL;
    
    return true;
}
 
// ===========================================
// FORMAT
// ===========================================
 
bool minimafs_format(void* device_handle, uint64_t size, 
                    uint8_t drive_number, const char* drive_name) {
    serial_write_str("MinimaFS: Formatting drive ");
    serial_write_dec(drive_number);
    serial_write_str(" (");
    serial_write_dec(size / (1024 * 1024));
    serial_write_str(" MB)\n");
    
    // Calculate blocks
    uint32_t total_blocks = size / MINIMAFS_BLOCK_SIZE;
    if (total_blocks > (MINIMAFS_BITMAP_SIZE * 8)) {
        serial_write_str("MinimaFS: Drive too large for bitmap\n");
        return false;
    }
    
    // Initialize block allocator
    block_alloc_init(drive_number, total_blocks);
    
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive) return false;
    drive->device_handle = device_handle;
    drive->drive_number = drive_number;
    
    // Create storage.desc
    minimafs_storage_desc_t storage_desc;
    memset(&storage_desc, 0, sizeof(storage_desc));
    
    storage_desc.magic = MINIMAFS_MAGIC;
    storage_desc.drive_number = drive_number;
    
    if (drive_name) {
        strcpy(storage_desc.drive_name, drive_name);
    } else {
        sprintf(storage_desc.drive_name, "%u", drive_number);
    }
    
    storage_desc.total_size = size;
    storage_desc.used_size = MINIMAFS_BLOCK_SIZE;  // storage.desc
    storage_desc.free_size = size - MINIMAFS_BLOCK_SIZE;
    
    storage_desc.total_blocks = total_blocks;
    
    // Allocate root folder block
    uint32_t root_block = block_alloc(drive_number);
    if (root_block == 0xFFFFFFFF) {
        return false;
    }
    storage_desc.root_block = root_block;
    
    storage_desc.used_blocks = 2;  // storage.desc + root folder
    storage_desc.free_blocks = total_blocks - 2;
    storage_desc.used_size = (uint64_t)storage_desc.used_blocks * MINIMAFS_BLOCK_SIZE;
    storage_desc.free_size = size - storage_desc.used_size;
    
    minimafs_get_datetime(storage_desc.created_date, sizeof(storage_desc.created_date));
    minimafs_get_datetime(storage_desc.last_mounted, sizeof(storage_desc.last_mounted));
    
    if (!minimafs_write_storage_desc(drive, &storage_desc)) {
        serial_write_str("MinimaFS: Failed to write storage.desc\n");
        return false;
    }
    
    // Read-back verify
    minimafs_storage_desc_t verify_desc;
    if (!minimafs_read_storage_desc(drive, &verify_desc)) {
        serial_write_str("MinimaFS: storage.desc verify failed\n");
        return false;
    }
    
    // Create root folder.desc
    minimafs_folder_desc_t root_desc;
    memset(&root_desc, 0, sizeof(root_desc));
    strcpy(root_desc.path, "/");
    root_desc.block_offset = root_block;
    root_desc.entry_count = 0;
    
    if (!minimafs_write_folder_desc(drive, &root_desc)) {
        serial_write_str("MinimaFS: Failed to write root folder.desc\n");
        return false;
    }
    
    drive->storage_desc = storage_desc;
    
    serial_write_str("MinimaFS: Format complete\n");
    
    return true;
}
 
// ===========================================
// METADATA OPERATIONS
// ===========================================
 
bool minimafs_get_metadata(const char* path, minimafs_file_metadata_t* metadata) {
    if (!metadata) return false;
    minimafs_file_handle_t* handle = minimafs_open(path, true);
    if (!handle) return false;
    *metadata = handle->metadata;
    minimafs_close(handle);
    return true;
}
 
bool minimafs_set_metadata(const char* path, const minimafs_file_metadata_t* metadata) {
    if (!metadata) return false;
    minimafs_file_handle_t* handle = minimafs_open(path, false);
    if (!handle) return false;
    
    minimafs_file_metadata_t updated = handle->metadata;
    strcpy(updated.filetype, metadata->filetype);
    strcpy(updated.fileformat, metadata->fileformat);
    updated.runnable = metadata->runnable;
    updated.entrypoint = metadata->entrypoint;
    strcpy(updated.run_with, metadata->run_with);
    updated.hidden = metadata->hidden;
    minimafs_get_datetime(updated.last_changed, sizeof(updated.last_changed));
    
    bool ok = true;
    minimafs_drive_t* drive = get_drive(handle->drive_number);
    if (drive && drive->mounted) {
        char local_path[MINIMAFS_MAX_PATH];
        uint8_t dn;
        if (minimafs_parse_path(handle->path, &dn, local_path)) {
            ok = minimafs_write_file_to_disk(drive, local_path, &updated, handle->data, handle->data_size);
            if (ok) {
                char filename[MINIMAFS_MAX_FILENAME];
                char parent[MINIMAFS_MAX_PATH];
                minimafs_split_local_path(local_path, parent, filename);
                minimafs_folder_desc_t parent_desc;
                if (minimafs_read_folder_desc(drive, parent, &parent_desc)) {
                    uint32_t idx = 0;
                    if (minimafs_find_entry_in_folder(&parent_desc, filename, &idx)) {
                        parent_desc.entries[idx].hidden = updated.hidden;
                        minimafs_write_folder_desc(drive, &parent_desc);
                    }
                }
            }
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    
    minimafs_close(handle);
    return ok;
}
 
bool minimafs_get_storage_desc(uint8_t drive_number, minimafs_storage_desc_t* desc) {
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive || !drive->mounted) {
        return false;
    }
    
    *desc = drive->storage_desc;
    return true;
}
