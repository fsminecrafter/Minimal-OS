#include "x86_64/minimafs.h"
#include "serial.h"
#include "string.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "time.h"
#include "x86_64/ahci.h"
#include "x86_64/exec_trace.h"
#include "x86_64/safeints.h"

/* Disk device wrapper */
typedef struct {
    ahci_drive_t* ahci_drive;
    uint32_t      sector_size;
} minimafs_disk_device_t;

/* ================================================================
 * HEAP-ALLOCATION HELPERS
 * These exist solely to keep the large structs off the stack.
 * ================================================================ */

#define ALLOC_FOLDER_DESC(var)                                          \
    minimafs_folder_desc_t* var =                                       \
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t)); \
    if (!(var)) {                                                       \
        serial_write_str("MinimaFS: OOM allocating folder_desc\n");     \
        return false;                                                   \
    }

/* Variant that returns NULL instead of false */
#define ALLOC_FOLDER_DESC_NULL(var)                                     \
    minimafs_folder_desc_t* var =                                       \
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t)); \
    if (!(var)) {                                                       \
        serial_write_str("MinimaFS: OOM allocating folder_desc\n");     \
        return NULL;                                                    \
    }

/* Variant that returns 0 instead of false */
#define ALLOC_FOLDER_DESC_ZERO(var)                                     \
    minimafs_folder_desc_t* var =                                       \
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t)); \
    if (!(var)) {                                                       \
        serial_write_str("MinimaFS: OOM allocating folder_desc\n");     \
        return 0;                                                       \
    }

#define FREE_FOLDER_DESC(var)  do { if (var) { free_mem(var); (var) = NULL; } } while(0)

/* ================================================================
 * GLOBAL STATE
 * ================================================================ */

static minimafs_drive_t g_drives[MINIMAFS_MAX_DRIVES];
static bool             g_initialized = false;

#define MINIMAFS_DMA_BOUNCE_BLOCKS  16
static void* g_dma_bounce[MINIMAFS_MAX_DRIVES];
static bool  g_self_test_done[MINIMAFS_MAX_DRIVES];

#define MINIMAFS_ENABLE_SELF_TEST  1
#define MINIMAFS_MAX_ENTRIES       4096
#define MINIMAFS_DEBUG_IO          1

/* ================================================================
 * FORWARD DECLARATIONS
 * ================================================================ */

static bool     minimafs_read_blocks(minimafs_drive_t*, uint32_t, uint32_t, void*);
static bool     minimafs_write_blocks(minimafs_drive_t*, uint32_t, uint32_t, const void*);
static bool     minimafs_self_test_drive(minimafs_drive_t*);
static uint32_t block_alloc_run(uint8_t, uint32_t);
static void     block_free_run(uint8_t, uint32_t, uint32_t);
bool            minimafs_read_folder_desc(minimafs_drive_t*, const char*, minimafs_folder_desc_t*);
bool            minimafs_write_folder_desc(minimafs_drive_t*, minimafs_folder_desc_t*);
bool            minimafs_write_storage_desc(minimafs_drive_t*);
bool            minimafs_storage_add_entry(minimafs_drive_t*, minimafs_dir_entry_t*);
static bool     minimafs_read_storage_desc(minimafs_drive_t*, minimafs_storage_desc_t*);
void            minimafs_refresh_storage_desc(minimafs_drive_t*);

/* ================================================================
 * INTERNAL HELPERS
 * ================================================================ */

static inline bool minimafs_irq_enabled(uint64_t flags) {
    return (flags & (1ULL << 9)) != 0;
}

static void minimafs_sleep_ms(uint32_t ms) {
    uint64_t flags = irq_save(__FILE__, "minimafs_sleep_ms", __LINE__);
    bool ints_enabled = minimafs_irq_enabled(flags);
    irq_restore(flags, __FILE__, "minimafs_sleep_ms", __LINE__);

    if (ints_enabled) {
        uint64_t start = time_get_uptime_ms();
        while (time_get_uptime_ms() - start < ms)
            asm volatile("hlt");
    } else {
        for (volatile uint32_t i = 0; i < (ms * 10000u); i++)
            asm volatile("nop");
    }
}

minimafs_drive_t* get_drive(uint8_t drive_number) {
    if (drive_number >= MINIMAFS_MAX_DRIVES) return NULL;
    return &g_drives[drive_number];
}

static bool ensure_dma_bounce(uint8_t drive_number) {
    if (drive_number >= MINIMAFS_MAX_DRIVES) return false;
    if (g_dma_bounce[drive_number])          return true;

    serial_write_str("MinimaFS: Allocating DMA bounce buffer\n");

    void* base = alloc_pages_zeroed(MINIMAFS_DMA_BOUNCE_BLOCKS);
    if (!base) {
        serial_write_str("MinimaFS: alloc_pages_zeroed failed, trying single page\n");
        base = alloc_page_zeroed();
        if (!base) {
            serial_write_str("MinimaFS: even single page failed!\n");
            return false;
        }
    }

    if ((uintptr_t)base & 0xFFF) {
        serial_write_str("MinimaFS: ERROR DMA buffer misaligned!\n");
        return false;
    }

    g_dma_bounce[drive_number] = base;

    serial_write_str("MinimaFS: DMA bounce buffer at 0x");
    serial_write_hex((uintptr_t)base);
    serial_write_str("\n");
    return true;
}

/* ================================================================
 * INITIALIZATION
 * ================================================================ */

void minimafs_init(void) {
    serial_write_str("MinimaFS: Initializing...\n");

    memset(g_drives,        0, sizeof(g_drives));
    memset(g_dma_bounce,    0, sizeof(g_dma_bounce));
    memset(g_self_test_done,0, sizeof(g_self_test_done));

    for (int i = 0; i < MINIMAFS_MAX_DRIVES; i++) {
        g_drives[i].mounted      = false;
        g_drives[i].drive_number = (uint8_t)i;
    }

    g_initialized = true;
    serial_write_str("MinimaFS: Ready\n");
}

/* ================================================================
 * DATE/TIME HELPER
 * ================================================================ */

void minimafs_get_datetime(char* buffer, size_t size) {
    datetime_t dt = time_get_datetime();
    const char* mon = time_get_month_name_short(dt.month);
    if (!mon || mon[0] == '\0') mon = "???";
    if (size < 10) return;

    buffer[0] = '0' + (dt.day / 10);
    buffer[1] = '0' + (dt.day % 10);
    buffer[2] = mon[0];
    buffer[3] = mon[1];
    buffer[4] = mon[2];
    buffer[5] = '0' + ((dt.year / 1000) % 10);
    buffer[6] = '0' + ((dt.year / 100)  % 10);
    buffer[7] = '0' + ((dt.year / 10)   % 10);
    buffer[8] = '0' + (dt.year % 10);
    buffer[9] = '\0';
}

/* ================================================================
 * PATH PARSING
 * ================================================================ */

bool minimafs_parse_path(const char* path, uint8_t* drive_number, char* local_path) {
    if (!path || !drive_number || !local_path) return false;

    const char* colon = strchr(path, ':');
    if (!colon) {
        serial_write_str("MinimaFS: Invalid path (missing ':'): ");
        serial_write_str(path);
        serial_write_str("\n");
        return false;
    }

    size_t drive_len = (size_t)(colon - path);
    if (drive_len == 0 || drive_len > 63) return false;

    char drive_str[64];
    memcpy(drive_str, path, drive_len);
    drive_str[drive_len] = '\0';

    if (drive_str[0] >= '0' && drive_str[0] <= '9') {
        *drive_number = 0;
        for (size_t i = 0; i < drive_len; i++) {
            if (drive_str[i] < '0' || drive_str[i] > '9') return false;
            *drive_number = (uint8_t)((*drive_number * 10) + (drive_str[i] - '0'));
        }
        if (*drive_number >= MINIMAFS_MAX_DRIVES) return false;
    } else {
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

    strcpy(local_path, colon + 1);
    return true;
}

/* ================================================================
 * FILE FORMAT PARSING
 * ================================================================ */

static bool parse_tag(const char* line, const char* tag, char* value, size_t max_len) {
    char tag_start[128];
    snprintf(tag_start, sizeof(tag_start), "@%s:", tag);

    const char* start = strstr(line, tag_start);
    if (!start) return false;
    start += strlen(tag_start);

    if (*start == '\'') {
        start++;
        const char* end = strchr(start, '\'');
        if (!end) return false;
        size_t len = (size_t)(end - start);
        if (len >= max_len) len = max_len - 1;
        memcpy(value, start, len);
        value[len] = '\0';
        return true;
    } else {
        const char* end = strchr(start, '@');
        if (!end) return false;
        size_t len = (size_t)(end - start);
        if (len >= max_len) len = max_len - 1;
        memcpy(value, start, len);
        value[len] = '\0';
        while (len > 0 && (value[len-1]==' '||value[len-1]=='\t'))
            value[--len] = '\0';
        return true;
    }
}

static bool parse_tag_bool(const char* line, const char* tag) {
    char value[16];
    if (!parse_tag(line, tag, value, sizeof(value))) return false;
    return strcmp(value,"True")==0 || strcmp(value,"true")==0 || strcmp(value,"1")==0;
}

static uint64_t parse_tag_uint64(const char* line, const char* tag) {
    char value[32];
    if (!parse_tag(line, tag, value, sizeof(value))) return 0;

    if (value[0]=='0' && (value[1]=='x'||value[1]=='X')) {
        uint64_t r = 0;
        for (int i = 2; value[i]; i++) {
            r <<= 4;
            if (value[i]>='0'&&value[i]<='9') r += value[i]-'0';
            else if (value[i]>='a'&&value[i]<='f') r += value[i]-'a'+10;
            else if (value[i]>='A'&&value[i]<='F') r += value[i]-'A'+10;
        }
        return r;
    }

    uint64_t r = 0;
    for (int i = 0; value[i]; i++)
        if (value[i]>='0'&&value[i]<='9') r = r*10 + (value[i]-'0');
    return r;
}

bool minimafs_parse_file_header(const char* data, minimafs_file_metadata_t* metadata) {
    if (!data || !metadata) return false;
    if (strncmp(data, "@HEADER@", 8) != 0) {
        serial_write_str("MinimaFS: Missing @HEADER@\n");
        return false;
    }

    memset(metadata, 0, sizeof(minimafs_file_metadata_t));

    const char* line = data;
    while (*line) {
        const char* line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);

        if (strncmp(line, "@DATA@", 6) == 0) break;

        if (strstr(line, "@FILETYPE:"))     parse_tag(line,"FILETYPE",   metadata->filetype,       sizeof(metadata->filetype));
        if (strstr(line, "@FILEFORMAT:"))   parse_tag(line,"FILEFORMAT", metadata->fileformat,     sizeof(metadata->fileformat));
        if (strstr(line, "@FILELEN:"))      metadata->file_length  = (uint32_t)parse_tag_uint64(line,"FILELEN");
        if (strstr(line, "@DATALEN:"))      metadata->data_length  = (uint32_t)parse_tag_uint64(line,"DATALEN");
        if (strstr(line, "@FILENAME:"))     parse_tag(line,"FILENAME",   metadata->filename,       sizeof(metadata->filename));
        if (strstr(line, "@CREATEDDATE:"))  parse_tag(line,"CREATEDDATE",metadata->created_date,  sizeof(metadata->created_date));
        if (strstr(line, "@LASTCHANGED:"))  parse_tag(line,"LASTCHANGED",metadata->last_changed,  sizeof(metadata->last_changed));
        if (strstr(line, "@PARENTFOLDER:")) parse_tag(line,"PARENTFOLDER",metadata->parent_folder,sizeof(metadata->parent_folder));
        if (strstr(line, "@RUNNABLE:"))     metadata->runnable    = parse_tag_bool(line,"RUNNABLE");
        if (strstr(line, "@ENTRYPOINT:"))   metadata->entrypoint  = parse_tag_uint64(line,"ENTRYPOINT");
        if (strstr(line, "@RUNWITH:"))      parse_tag(line,"RUNWITH",    metadata->run_with,       sizeof(metadata->run_with));
        if (strstr(line, "@HIDDEN:"))       metadata->hidden      = parse_tag_bool(line,"HIDDEN");

        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }
    return true;
}

/* ================================================================
 * FILE FORMAT GENERATION
 * ================================================================ */

static char* minimafs_generate_file_header(const minimafs_file_metadata_t* metadata,
                                           uint32_t* header_size) {
    char* header = (char*)alloc_unzeroed(2048);
    if (!header) return NULL;

    char* ptr = header;
    ptr += sprintf(ptr, "@HEADER@\n");
    ptr += sprintf(ptr, "@FILETYPE:%s@\n",        metadata->filetype);
    ptr += sprintf(ptr, "@FILEFORMAT:%s@\n",      metadata->fileformat);
    ptr += sprintf(ptr, "@FILELEN:%u@\n",         metadata->file_length);
    ptr += sprintf(ptr, "@DATALEN:%u@\n",         metadata->data_length);
    ptr += sprintf(ptr, "@FILENAME:'%s'@\n",      metadata->filename);
    ptr += sprintf(ptr, "@CREATEDDATE:'%s'@\n",   metadata->created_date);
    ptr += sprintf(ptr, "@LASTCHANGED:'%s'@\n",   metadata->last_changed);
    ptr += sprintf(ptr, "@PARENTFOLDER:'%s'@\n",  metadata->parent_folder);

    if (metadata->runnable) {
        ptr += sprintf(ptr, "@RUNNABLE:True@\n");
        if (metadata->entrypoint != 0) {
            char hexbuf[32];
            hex_to_str(metadata->entrypoint, hexbuf);
            ptr += sprintf(ptr, "@ENTRYPOINT:0x%s@\n", hexbuf);
        }
        if (metadata->run_with[0] != '\0')
            ptr += sprintf(ptr, "@RUNWITH:%s@\n", metadata->run_with);
    } else {
        ptr += sprintf(ptr, "@RUNNABLE:False@\n");
    }

    ptr += sprintf(ptr, "@HIDDEN:%s@\n", metadata->hidden ? "True" : "False");
    ptr += sprintf(ptr, "@DATA@\n");

    *header_size = (uint32_t)(ptr - header);
    return header;
}

static void minimafs_copy_segment_to_block(uint8_t* block, uint32_t block_offset,
                                           const uint8_t* src, uint32_t src_len,
                                           uint32_t src_start) {
    if (!block || !src || src_len == 0) return;
    uint32_t block_end = block_offset + MINIMAFS_BLOCK_SIZE;
    uint32_t src_end   = src_start + src_len;
    if (src_end <= block_offset || src_start >= block_end) return;
    uint32_t copy_start = (src_start > block_offset) ? src_start : block_offset;
    uint32_t copy_end   = (src_end   < block_end)    ? src_end   : block_end;
    uint32_t copy_len   = copy_end - copy_start;
    memcpy(block + (copy_start - block_offset),
           src   + (copy_start - src_start),
           copy_len);
}

/* ================================================================
 * BLOCK I/O
 * ================================================================ */

static bool minimafs_read_blocks(minimafs_drive_t* drive, uint32_t block_num,
                                 uint32_t count, void* buffer) {
    if (!drive || !drive->device_handle) return false;
    minimafs_disk_device_t* disk = (minimafs_disk_device_t*)drive->device_handle;
    if (!disk->ahci_drive || disk->sector_size == 0) return false;
    if (!ensure_dma_bounce(drive->drive_number)) return false;
    if (!buffer || count == 0) return count == 0;

    uint8_t*  out       = (uint8_t*)buffer;
    uint8_t*  bounce    = (uint8_t*)g_dma_bounce[drive->drive_number];
    uint32_t  remaining = count;
    uint32_t  cur_block = block_num;

    while (remaining > 0) {
        uint32_t chunk = (remaining > MINIMAFS_DMA_BOUNCE_BLOCKS) ?
                          MINIMAFS_DMA_BOUNCE_BLOCKS : remaining;

        uint32_t sectors_per_block = MINIMAFS_BLOCK_SIZE / disk->sector_size;
        uint64_t start_sector      = (uint64_t)cur_block * sectors_per_block;
        uint32_t sector_count      = chunk * sectors_per_block;

        if (!ahci_read(disk->ahci_drive, start_sector, sector_count, bounce))
            return false;

        memcpy(out, bounce, chunk * MINIMAFS_BLOCK_SIZE);
        out       += chunk * MINIMAFS_BLOCK_SIZE;
        cur_block += chunk;
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
    if (!buffer || count == 0) return count == 0;
    if ((MINIMAFS_BLOCK_SIZE % disk->sector_size) != 0) return false;

    const uint8_t* in         = (const uint8_t*)buffer;
    uint8_t*       bounce     = (uint8_t*)g_dma_bounce[drive->drive_number];
    uint32_t       remaining  = count;
    uint32_t       cur_block  = block_num;

    const uint32_t MAX_CHUNK  = 4;   /* 16 KB at a time – keeps AHCI happy */

    while (remaining > 0) {
        uint32_t chunk = (remaining > MAX_CHUNK) ? MAX_CHUNK : remaining;

        memcpy(bounce, in, chunk * MINIMAFS_BLOCK_SIZE);

        uint32_t sectors_per_block = MINIMAFS_BLOCK_SIZE / disk->sector_size;
        uint64_t start_sector      = (uint64_t)cur_block * sectors_per_block;
        uint32_t sector_count      = chunk * sectors_per_block;

        /* Enable interrupts around the AHCI call so the PIT/scheduler
         * can still tick while we wait for DMA completion. */
        uint64_t flags = irq_save(__FILE__, "minimafs_write_blocks", __LINE__);
        bool ok = ahci_write(disk->ahci_drive, start_sector, sector_count, bounce);
        irq_restore(flags, __FILE__, "minimafs_write_blocks", __LINE__);

        if (!ok) {
            serial_write_str("MinimaFS: write_blocks AHCI error at block ");
            serial_write_dec(cur_block);
            serial_write_str("\n");
            return false;
        }

        in        += chunk * MINIMAFS_BLOCK_SIZE;
        cur_block += chunk;
        remaining -= chunk;

        /* Brief delay between chunks */
        if (remaining > 0)
            minimafs_sleep_ms(5);
    }
    return true;
}

/* ================================================================
 * BLOCK ALLOCATOR
 * ================================================================ */

#define MINIMAFS_BITMAP_SIZE 8192  /* 64 K blocks = 256 MB max */

typedef struct {
    uint8_t  bitmap[MINIMAFS_BITMAP_SIZE];
    uint32_t total_blocks;
    uint32_t free_blocks;
} minimafs_block_alloc_t;

static minimafs_block_alloc_t g_block_alloc[MINIMAFS_MAX_DRIVES];

static void block_alloc_init(uint8_t drive_num, uint32_t total_blocks) {
    minimafs_block_alloc_t* a = &g_block_alloc[drive_num];
    memset(a->bitmap, 0, MINIMAFS_BITMAP_SIZE);
    a->total_blocks = total_blocks;
    a->free_blocks  = total_blocks;
    /* block 0 = storage.desc, always used */
    a->bitmap[0] |= 1;
    a->free_blocks--;
}

static void block_alloc_mark_used(uint8_t drive_num, uint32_t block_num, uint32_t count) {
    minimafs_block_alloc_t* a = &g_block_alloc[drive_num];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b    = block_num + i;
        uint32_t byte = b / 8;
        uint8_t  bit  = b % 8;
        if (byte >= MINIMAFS_BITMAP_SIZE) return;
        if (!(a->bitmap[byte] & (1 << bit))) {
            a->bitmap[byte] |= (1 << bit);
            if (a->free_blocks > 0) a->free_blocks--;
        }
    }
}

static uint32_t block_alloc_run(uint8_t drive_num, uint32_t count) {
    if (count == 0) return 0xFFFFFFFF;
    minimafs_block_alloc_t* a = &g_block_alloc[drive_num];
    uint32_t max_b = MINIMAFS_BITMAP_SIZE * 8;
    uint32_t run_start = 0, run_len = 0;

    for (uint32_t b = 1; b < max_b; b++) {
        bool used = (a->bitmap[b/8] & (1 << (b%8))) != 0;
        if (!used) {
            if (run_len == 0) run_start = b;
            if (++run_len >= count) {
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
    minimafs_block_alloc_t* a = &g_block_alloc[drive_num];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b    = block_num + i;
        uint32_t byte = b / 8;
        uint8_t  bit  = b % 8;
        if (byte >= MINIMAFS_BITMAP_SIZE) return;
        if (a->bitmap[byte] & (1 << bit)) {
            a->bitmap[byte] &= ~(1 << bit);
            a->free_blocks++;
        }
    }
}

/* ================================================================
 * STORAGE.DESC
 * ================================================================ */

bool minimafs_write_storage_desc(minimafs_drive_t* drive) {
    if (!drive) return false;
    minimafs_storage_desc_t* sd = &drive->storage_desc;

    /* Use heap for the 4KB block buffer */
    char* buffer = (char*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE);
    if (!buffer) { serial_write_str("ERROR: OOM writing storage.desc\n"); return false; }
    memset(buffer, 0, MINIMAFS_BLOCK_SIZE);
    char* ptr = buffer;

    ptr += sprintf(ptr, "@MAGIC:%u@\n",            sd->magic);
    ptr += sprintf(ptr, "@DRIVE_NUMBER:%u@\n",     sd->drive_number);
    ptr += sprintf(ptr, "@DRIVE_NAME:'%s'@\n",     sd->drive_name);
    ptr += sprintf(ptr, "@PASSWORD:'%s'@\n",       sd->password);
    ptr += sprintf(ptr, "@PASSWORDPROTECTED:%s@\n",sd->password_protected ? "True" : "False");
    ptr += sprintf(ptr, "@TOTAL_SIZE_HI:%u@\n",   (uint32_t)(sd->total_size >> 32));
    ptr += sprintf(ptr, "@TOTAL_SIZE_LO:%u@\n",   (uint32_t)(sd->total_size & 0xFFFFFFFF));
    ptr += sprintf(ptr, "@USED_SIZE_HI:%u@\n",    (uint32_t)(sd->used_size  >> 32));
    ptr += sprintf(ptr, "@USED_SIZE_LO:%u@\n",    (uint32_t)(sd->used_size  & 0xFFFFFFFF));
    ptr += sprintf(ptr, "@FREE_SIZE_HI:%u@\n",    (uint32_t)(sd->free_size  >> 32));
    ptr += sprintf(ptr, "@FREE_SIZE_LO:%u@\n",    (uint32_t)(sd->free_size  & 0xFFFFFFFF));
    ptr += sprintf(ptr, "@TOTAL_BLOCKS:%u@\n",     sd->total_blocks);
    ptr += sprintf(ptr, "@USED_BLOCKS:%u@\n",      sd->used_blocks);
    ptr += sprintf(ptr, "@FREE_BLOCKS:%u@\n",      sd->free_blocks);
    ptr += sprintf(ptr, "@ROOT_BLOCK:%u@\n",       sd->root_block);
    ptr += sprintf(ptr, "@FILESYSTEM_LABEL:'%s'@\n",sd->filesystem_label);
    ptr += sprintf(ptr, "@CREATEDDATE:'%s'@\n",    sd->created_date);
    ptr += sprintf(ptr, "@LASTMOUNTED:'%s'@\n",    sd->last_mounted);
    ptr += sprintf(ptr, "@END@\n");

    if ((size_t)(ptr - buffer) >= MINIMAFS_BLOCK_SIZE) {
        serial_write_str("ERROR: storage.desc too large\n");
        free_mem(buffer);
        return false;
    }

    bool ok = minimafs_write_blocks(drive, 0, 1, buffer);
    free_mem(buffer);
    if (ok) serial_write_str("MinimaFS: storage.desc written\n");
    return ok;
}

static bool minimafs_read_storage_desc(minimafs_drive_t* drive,
                                       minimafs_storage_desc_t* desc) {
    if (!drive || !desc) return false;

    char* buffer = (char*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE + 1);
    if (!buffer) { serial_write_str("MinimaFS: OOM reading storage.desc\n"); return false; }

    if (!minimafs_read_blocks(drive, 0, 1, buffer)) {
        serial_write_str("MinimaFS: Read storage.desc failed\n");
        free_mem(buffer);
        return false;
    }
    buffer[MINIMAFS_BLOCK_SIZE] = '\0';

    memset(desc, 0, sizeof(minimafs_storage_desc_t));

    desc->magic           = (uint32_t)parse_tag_uint64(buffer, "MAGIC");
    desc->drive_number    = (uint8_t) parse_tag_uint64(buffer, "DRIVE_NUMBER");
    parse_tag(buffer, "DRIVE_NAME",        desc->drive_name,       sizeof(desc->drive_name));
    parse_tag(buffer, "PASSWORD",          desc->password,         sizeof(desc->password));
    desc->password_protected = parse_tag_bool(buffer, "PASSWORDPROTECTED");
    desc->total_size  = ((uint64_t)parse_tag_uint64(buffer,"TOTAL_SIZE_HI")<<32)
                       | (uint64_t)parse_tag_uint64(buffer,"TOTAL_SIZE_LO");
    desc->used_size   = ((uint64_t)parse_tag_uint64(buffer,"USED_SIZE_HI")<<32)
                       | (uint64_t)parse_tag_uint64(buffer,"USED_SIZE_LO");
    desc->free_size   = ((uint64_t)parse_tag_uint64(buffer,"FREE_SIZE_HI")<<32)
                       | (uint64_t)parse_tag_uint64(buffer,"FREE_SIZE_LO");
    desc->total_blocks = (uint32_t)parse_tag_uint64(buffer,"TOTAL_BLOCKS");
    desc->used_blocks  = (uint32_t)parse_tag_uint64(buffer,"USED_BLOCKS");
    desc->free_blocks  = (uint32_t)parse_tag_uint64(buffer,"FREE_BLOCKS");
    desc->root_block   = (uint32_t)parse_tag_uint64(buffer,"ROOT_BLOCK");
    parse_tag(buffer, "FILESYSTEM_LABEL", desc->filesystem_label, sizeof(desc->filesystem_label));
    parse_tag(buffer, "CREATEDDATE",      desc->created_date,     sizeof(desc->created_date));
    parse_tag(buffer, "LASTMOUNTED",      desc->last_mounted,     sizeof(desc->last_mounted));

    if (desc->magic != MINIMAFS_MAGIC || desc->total_blocks == 0 ||
        desc->total_size == 0 || desc->root_block == 0) {
        serial_write_str("MinimaFS: storage.desc invalid\n");
        free_mem(buffer);
        return false;
    }

    free_mem(buffer);
    return true;
}

void minimafs_refresh_storage_desc(minimafs_drive_t* drive) {
    if (!drive) return;

    /* Use heap for the storage_desc to avoid the 6KB stack hit */
    minimafs_storage_desc_t* desc =
        (minimafs_storage_desc_t*)alloc(sizeof(minimafs_storage_desc_t));
    if (!desc) { serial_write_str("ERROR: OOM refresh_storage_desc\n"); return; }

    if (!minimafs_read_storage_desc(drive, desc)) {
        serial_write_str("ERROR: Failed to read storage.desc for refresh\n");
        free_mem(desc);
        return;
    }

    minimafs_block_alloc_t* ba = &g_block_alloc[drive->drive_number];
    desc->free_blocks = ba->free_blocks;
    desc->used_blocks = ba->total_blocks - ba->free_blocks;
    desc->used_size   = (uint64_t)desc->used_blocks * MINIMAFS_BLOCK_SIZE;
    desc->free_size   = desc->total_size - desc->used_size;
    minimafs_get_datetime(desc->last_mounted, sizeof(desc->last_mounted));

    if (desc->magic != MINIMAFS_MAGIC || desc->total_blocks == 0) {
        serial_write_str("ERROR: Refusing to write invalid storage.desc\n");
        free_mem(desc);
        return;
    }

    drive->storage_desc = *desc;
    free_mem(desc);
    minimafs_write_storage_desc(drive);
}

bool minimafs_storage_add_entry(minimafs_drive_t* drive, minimafs_dir_entry_t* entry) {
    if (!drive || !entry) return false;
    return minimafs_write_storage_desc(drive);
}

/* ================================================================
 * FOLDER.DESC PARSING
 * ================================================================ */

static uint32_t parse_uint32(const char* str) {
    uint32_t r = 0;
    if (!str) return 0;
    while (*str && (*str < '0' || *str > '9')) str++;
    while (*str >= '0' && *str <= '9') r = r * 10 + (*str++ - '0');
    return r;
}

static bool parse_folder_entry_line(const char* line, minimafs_dir_entry_t* entry) {
    if (!line || !entry || strncmp(line, "ENTRY:", 6) != 0) return false;
    const char* p     = line + 6;
    const char* comma = strchr(p, ',');
    if (!comma) return false;

    size_t name_len = (size_t)(comma - p);
    if (name_len >= MINIMAFS_MAX_ENTRY_NAME) name_len = MINIMAFS_MAX_ENTRY_NAME - 1;
    memcpy(entry->name, p, name_len);
    entry->name[name_len] = '\0';

    const char* type_start = comma + 1;
    const char* block_tag  = strstr(type_start, ",BLOCK:");
    if (!block_tag) return false;

    size_t type_len = (size_t)(block_tag - type_start);
    if (type_len >= 8) type_len = 7;
    char type_buf[8];
    memcpy(type_buf, type_start, type_len);
    type_buf[type_len] = '\0';

    entry->type         = (strcmp(type_buf, "DIR") == 0) ? MINIMAFS_TYPE_DIR : MINIMAFS_TYPE_FILE;
    entry->block_offset = parse_uint32(block_tag + 7);
    entry->block_count  = 1;
    entry->hidden       = false;

    const char* count_tag  = strstr(block_tag, ",COUNT:");
    if (count_tag) entry->block_count = parse_uint32(count_tag + 7);

    const char* hidden_tag = strstr(block_tag, ",HIDDEN:");
    if (hidden_tag) entry->hidden = parse_uint32(hidden_tag + 8) != 0;

    return true;
}

static bool minimafs_read_folder_desc_block(minimafs_drive_t* drive, uint32_t block,
                                            minimafs_folder_desc_t* desc) {
    char* buffer = (char*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE + 1);
    if (!buffer) { serial_write_str("MinimaFS: OOM reading folder.desc\n"); return false; }
    if (!minimafs_read_blocks(drive, block, 1, buffer)) { free_mem(buffer); return false; }
    buffer[MINIMAFS_BLOCK_SIZE] = '\0';

    memset(desc, 0, sizeof(minimafs_folder_desc_t));
    desc->block_offset = block;
    uint32_t parsed_count = 0;

    const char* line = buffer;
    while (*line) {
        const char* line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);

        if (strncmp(line, "FOLDER:", 7) == 0) {
            size_t len = (size_t)(line_end - (line + 7));
            if (len >= MINIMAFS_MAX_PATH) len = MINIMAFS_MAX_PATH - 1;
            memcpy(desc->path, line + 7, len);
            desc->path[len] = '\0';
        } else if (strncmp(line, "ENTRY:", 6) == 0 && parsed_count < 256) {
            if (parse_folder_entry_line(line, &desc->entries[parsed_count]))
                parsed_count++;
        }

        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }

    desc->entry_count = parsed_count;
    free_mem(buffer);
    return true;
}

/* Walk path components to find the block number for a directory.
 * Uses heap-allocated desc per level – safe for any stack size. */
static bool minimafs_get_folder_block(minimafs_drive_t* drive, const char* path,
                                      uint32_t* out_block) {
    if (!drive || !path || !out_block) return false;

    if (path[0]=='\0' || (path[0]=='/'&&path[1]=='\0')) {
        *out_block = drive->storage_desc.root_block;
        return true;
    }

    /* Copy path so strtok can modify it */
    size_t plen = strlen(path);
    char*  temp = (char*)alloc_unzeroed(plen + 1);
    if (!temp) return false;
    strcpy(temp, path);
    if (temp[0] == '/') memmove(temp, temp + 1, plen);   /* strip leading / */

    uint32_t current_block = drive->storage_desc.root_block;
    char*    token         = strtok(temp, "/");

    /* Heap-allocate the desc used for each level */
    minimafs_folder_desc_t* desc =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!desc) { free_mem(temp); return false; }

    while (token) {
        if (!minimafs_read_folder_desc_block(drive, current_block, desc)) {
            free_mem(desc); free_mem(temp);
            return false;
        }

        bool found = false;
        for (uint32_t i = 0; i < desc->entry_count; i++) {
            if (desc->entries[i].type == MINIMAFS_TYPE_DIR &&
                strcmp(desc->entries[i].name, token) == 0) {
                current_block = desc->entries[i].block_offset;
                found = true;
                break;
            }
        }

        if (!found) { free_mem(desc); free_mem(temp); return false; }
        token = strtok(NULL, "/");
    }

    free_mem(desc);
    free_mem(temp);
    *out_block = current_block;
    return true;
}

bool minimafs_read_folder_desc(minimafs_drive_t* drive, const char* path,
                               minimafs_folder_desc_t* desc) {
    if (!drive || !desc || !path) return false;

    uint32_t block;
    if (!minimafs_get_folder_block(drive, path, &block)) return false;
    if (!minimafs_read_folder_desc_block(drive, block, desc)) return false;

    if (desc->path[0] == '\0')
        strncpy(desc->path, path, sizeof(desc->path) - 1);

    return true;
}

bool minimafs_write_folder_desc(minimafs_drive_t* drive, minimafs_folder_desc_t* desc) {
    if (!drive || !desc || desc->block_offset == 0) return false;

    char* buffer = (char*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE);
    if (!buffer) { serial_write_str("ERROR: OOM writing folder.desc\n"); return false; }
    memset(buffer, 0, MINIMAFS_BLOCK_SIZE);
    char* ptr = buffer;

    ptr += sprintf(ptr, "FOLDER:%s\n",   desc->path);
    ptr += sprintf(ptr, "ENTRIES:%u\n",  desc->entry_count);

    for (uint32_t i = 0; i < desc->entry_count; i++) {
        const minimafs_dir_entry_t* e = &desc->entries[i];
        if ((size_t)(ptr - buffer) > MINIMAFS_BLOCK_SIZE - 128) {
            serial_write_str("ERROR: folder.desc too large\n");
            free_mem(buffer);
            return false;
        }
        ptr += sprintf(ptr, "ENTRY:%s,%s,BLOCK:%u,COUNT:%u,HIDDEN:%u\n",
                       e->name,
                       e->type == MINIMAFS_TYPE_DIR ? "DIR" : "FILE",
                       e->block_offset, e->block_count,
                       e->hidden ? 1 : 0);
    }
    ptr += sprintf(ptr, "@END\n");

    bool ok = minimafs_write_blocks(drive, desc->block_offset, 1, buffer);
    free_mem(buffer);
    return ok;
}

/* ================================================================
 * SCAN DIRECTORY  (recursive – must heap-allocate desc)
 * ================================================================ */

static uint32_t minimafs_calc_file_block_count(minimafs_drive_t* drive,
                                               uint32_t block_offset) {
    char* buf = (char*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE + 1);
    if (!buf) return 1;
    if (!minimafs_read_blocks(drive, block_offset, 1, buf)) { free_mem(buf); return 1; }
    buf[MINIMAFS_BLOCK_SIZE] = '\0';

    minimafs_file_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    minimafs_parse_file_header(buf, &meta);
    free_mem(buf);

    uint32_t file_len = meta.file_length;
    if (file_len == 0) file_len = MINIMAFS_BLOCK_SIZE;
    uint32_t aligned = ((file_len + MINIMAFS_BLOCK_SIZE - 1) / MINIMAFS_BLOCK_SIZE)
                        * MINIMAFS_BLOCK_SIZE;
    return aligned / MINIMAFS_BLOCK_SIZE;
}

void minimafs_scan_directory(minimafs_drive_t* drive, uint32_t block) {
    /* Heap-allocate to avoid recursive stack explosion */
    minimafs_folder_desc_t* desc =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!desc) return;

    if (!minimafs_read_folder_desc_block(drive, block, desc)) { free_mem(desc); return; }

    uint32_t count = (desc->entry_count > MINIMAFS_MAX_ENTRIES) ?
                      MINIMAFS_MAX_ENTRIES : desc->entry_count;

    /* Collect subdir blocks before freeing desc (avoids recursive depth
     * being multiplied by the desc size). */
    uint32_t subdir_blocks[MINIMAFS_MAX_ROOT_ENTRIES];
    uint32_t subdir_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        minimafs_dir_entry_t* e = &desc->entries[i];
        uint32_t blocks_to_mark = e->block_count;
        if (blocks_to_mark == 0) {
            blocks_to_mark = (e->type == MINIMAFS_TYPE_DIR) ? 1
                : minimafs_calc_file_block_count(drive, e->block_offset);
        }
        block_alloc_mark_used(drive->drive_number, e->block_offset, blocks_to_mark);

        if (e->type == MINIMAFS_TYPE_DIR &&
            subdir_count < MINIMAFS_MAX_ROOT_ENTRIES) {
            subdir_blocks[subdir_count++] = e->block_offset;
        }
    }

    free_mem(desc);   /* free before recursing */

    for (uint32_t i = 0; i < subdir_count; i++)
        minimafs_scan_directory(drive, subdir_blocks[i]);
}

/* ================================================================
 * WRITE FILE TO DISK (segments version)
 * ================================================================ */

static bool minimafs_write_file_to_disk_segments(minimafs_drive_t* drive,
                                                 const char* local_path,
                                                 minimafs_file_metadata_t* metadata,
                                                 const void* seg1, uint32_t seg1_len,
                                                 const void* seg2, uint32_t seg2_len) {
    if (!drive || !metadata) return false;

    uint32_t data_size      = seg1_len + seg2_len;
    metadata->data_length   = data_size;
    metadata->file_length   = 0;

    serial_write_str("MinimaFS: Writing file, data_size=");
    serial_write_dec(data_size);
    serial_write_str("\n");

    uint32_t header_size = 0;
    uint32_t footer_size = 5; /* "@END\n" */

    /* Two passes to stabilise header size */
    for (int i = 0; i < 2; i++) {
        metadata->file_length = header_size + data_size + footer_size;
        char* tmp = minimafs_generate_file_header(metadata, &header_size);
        if (!tmp) return false;
        free_mem(tmp);
    }

    metadata->file_length = header_size + data_size + footer_size;
    char* header = minimafs_generate_file_header(metadata, &header_size);
    if (!header) return false;

    uint32_t total_size   = header_size + data_size + footer_size;
    uint32_t aligned_size = ((total_size + MINIMAFS_BLOCK_SIZE - 1) / MINIMAFS_BLOCK_SIZE)
                             * MINIMAFS_BLOCK_SIZE;
    uint32_t block_count  = aligned_size / MINIMAFS_BLOCK_SIZE;

    uint32_t start_block  = metadata->block_offset;
    uint32_t old_count    = metadata->block_count;
    uint32_t old_offset   = metadata->block_offset;

    if (start_block != 0 && old_count >= block_count) {
        if (old_count > block_count)
            block_free_run(drive->drive_number, old_offset + block_count,
                           old_count - block_count);
    } else {
        start_block = block_alloc_run(drive->drive_number, block_count);
        if (start_block == 0xFFFFFFFF) { free_mem(header); return false; }
        if (old_count > 0)
            block_free_run(drive->drive_number, old_offset, old_count);
        metadata->block_offset = start_block;
    }

    metadata->block_count  = block_count;
    metadata->block_offset = start_block;

    serial_write_str("MinimaFS: Writing blocks ");
    serial_write_dec(start_block);
    serial_write_str(" - ");
    serial_write_dec(start_block + block_count - 1);
    serial_write_str("\n");

    const uint8_t* data1   = (const uint8_t*)seg1;
    const uint8_t* data2   = (const uint8_t*)seg2;
    const uint8_t  footer[5] = {'@','E','N','D','\n'};

    /* Allocate ONE chunk buffer outside the loop */
    const uint32_t MAX_CHUNK        = 16; /* 64 KB */
    const uint32_t chunk_buf_bytes  = MINIMAFS_BLOCK_SIZE * MAX_CHUNK;
    uint8_t* chunk_buf = (uint8_t*)alloc_unzeroed(chunk_buf_bytes);
    if (!chunk_buf) {
        serial_write_str("MinimaFS: OOM chunk buffer\n");
        block_free_run(drive->drive_number, start_block, block_count);
        free_mem(header);
        return false;
    }

    for (uint32_t i = 0; i < block_count; ) {
        uint32_t chunk = block_count - i;
        if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;

        memset(chunk_buf, 0, chunk * MINIMAFS_BLOCK_SIZE);

        for (uint32_t j = 0; j < chunk; j++) {
            uint32_t bo  = (i + j) * MINIMAFS_BLOCK_SIZE;
            uint8_t* bp  = chunk_buf + j * MINIMAFS_BLOCK_SIZE;

            minimafs_copy_segment_to_block(bp, bo, (const uint8_t*)header, header_size, 0);
            minimafs_copy_segment_to_block(bp, bo, data1, seg1_len, header_size);
            minimafs_copy_segment_to_block(bp, bo, data2, seg2_len, header_size + seg1_len);
            minimafs_copy_segment_to_block(bp, bo, footer, footer_size,
                                           header_size + data_size);
        }

        if (!minimafs_write_blocks(drive, start_block + i, chunk, chunk_buf)) {
            serial_write_str("MinimaFS: Write failed at block ");
            serial_write_dec(start_block + i);
            serial_write_str("\n");
            free_mem(chunk_buf);
            block_free_run(drive->drive_number, start_block, block_count);
            free_mem(header);
            return false;
        }

        i += chunk;

        if ((i % 64) == 0 || i == block_count) {
            serial_write_str("MinimaFS: ");
            serial_write_dec((i * 100) / block_count);
            serial_write_str("% complete\n");
        }
    }

    free_mem(chunk_buf);
    free_mem(header);
    minimafs_refresh_storage_desc(drive);
    serial_write_str("MinimaFS: Write complete\n");
    return true;
}

static bool minimafs_write_file_to_disk(minimafs_drive_t* drive, const char* local_path,
                                        minimafs_file_metadata_t* metadata,
                                        const void* data, uint32_t data_size) {
    return minimafs_write_file_to_disk_segments(drive, local_path, metadata,
                                                data, data_size, NULL, 0);
}

/* ================================================================
 * PATH HELPERS
 * ================================================================ */

static void minimafs_split_local_path(const char* local_path, char* parent, char* name) {
    const char* last_slash = strrchr(local_path, '/');
    if (last_slash) {
        strcpy(name, last_slash + 1);
        size_t parent_len = (size_t)(last_slash - local_path);
        memcpy(parent, local_path, parent_len);
        parent[parent_len] = '\0';
    } else {
        strcpy(name, local_path);
        parent[0] = '\0';
    }
}

static bool minimafs_find_entry_in_folder(minimafs_folder_desc_t* desc,
                                          const char* name, uint32_t* index) {
    if (!desc || !name) return false;
    for (uint32_t i = 0; i < desc->entry_count; i++) {
        if (strcmp(desc->entries[i].name, name) == 0) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

/* ================================================================
 * FILE OPERATIONS  (all folder_desc on heap)
 * ================================================================ */

bool minimafs_create_file(const char* path, const char* filetype,
                          const char* fileformat) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        serial_write_str("MinimaFS: Drive not mounted\n");
        return false;
    }

    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* parent_desc =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!parent_desc) return false;

    if (!minimafs_read_folder_desc(drive, parent, parent_desc)) {
        serial_write_str("ERROR: Failed to read parent folder.desc\n");
        free_mem(parent_desc); return false;
    }

    uint32_t existing_index = 0;
    bool exists = minimafs_find_entry_in_folder(parent_desc, filename, &existing_index);
    if (exists && parent_desc->entries[existing_index].type != MINIMAFS_TYPE_DIR) {
        serial_write_str("ERROR: File already exists\n");
        free_mem(parent_desc); return false;
    }

    if (parent_desc->entry_count >= 256) {
        serial_write_str("ERROR: Parent folder full\n");
        free_mem(parent_desc); return false;
    }

    minimafs_file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    strcpy(metadata.filename,      filename);
    strcpy(metadata.filetype,      filetype);
    strcpy(metadata.fileformat,    fileformat);
    strcpy(metadata.parent_folder, parent);
    minimafs_get_datetime(metadata.created_date, sizeof(metadata.created_date));
    minimafs_get_datetime(metadata.last_changed, sizeof(metadata.last_changed));

    if (!minimafs_write_file_to_disk(drive, local_path, &metadata, NULL, 0)) {
        serial_write_str("ERROR: Failed to write file to disk\n");
        free_mem(parent_desc); return false;
    }

    minimafs_dir_entry_t* entry = &parent_desc->entries[parent_desc->entry_count];
    strcpy(entry->name, filename);
    entry->type         = MINIMAFS_TYPE_FILE;
    entry->block_offset = metadata.block_offset;
    entry->block_count  = metadata.block_count;
    entry->hidden       = false;
    parent_desc->entry_count++;

    if (!minimafs_write_folder_desc(drive, parent_desc)) {
        serial_write_str("ERROR: Failed to write updated folder.desc\n");
        free_mem(parent_desc); return false;
    }

    free_mem(parent_desc);
    serial_write_str("MinimaFS: File created: ");
    serial_write_str(path);
    serial_write_str("\n");
    return true;
}

minimafs_file_handle_t* minimafs_open(const char* path, bool read_only) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return NULL;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) {
        serial_write_str("MinimaFS: Drive not mounted\n");
        return NULL;
    }

    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* parent_desc =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!parent_desc) return NULL;

    if (!minimafs_read_folder_desc(drive, parent, parent_desc)) {
        free_mem(parent_desc); return NULL;
    }

    uint32_t entry_index = 0;
    if (!minimafs_find_entry_in_folder(parent_desc, filename, &entry_index)) {
        if (read_only) { free_mem(parent_desc); return NULL; }
        if (!minimafs_create_file(path, "binary", "bin")) {
            free_mem(parent_desc); return NULL;
        }
        /* Re-read after creation */
        if (!minimafs_read_folder_desc(drive, parent, parent_desc)) {
            free_mem(parent_desc); return NULL;
        }
        if (!minimafs_find_entry_in_folder(parent_desc, filename, &entry_index)) {
            free_mem(parent_desc); return NULL;
        }
    }

    minimafs_dir_entry_t entry = parent_desc->entries[entry_index];
    free_mem(parent_desc);   /* no longer needed */

    if (entry.type == MINIMAFS_TYPE_DIR) return NULL;

    minimafs_file_handle_t* handle =
        (minimafs_file_handle_t*)alloc_unzeroed(sizeof(minimafs_file_handle_t));
    if (!handle) return NULL;
    memset(handle, 0, sizeof(minimafs_file_handle_t));

    handle->open          = true;
    handle->drive_number  = drive_num;
    strcpy(handle->path, path);
    handle->read_only     = read_only;
    handle->position      = 0;
    handle->modified      = false;

    handle->file_block_offset  = entry.block_offset;
    handle->file_block_count   = entry.block_count;
    handle->data_offset_in_blocks = 0;
    handle->stream_cache       = NULL;
    handle->cached_block_index = (uint32_t)-1;
    handle->use_streaming      = false;

    uint32_t total_size = entry.block_count * MINIMAFS_BLOCK_SIZE;

    if (total_size == 0) {
        memset(&handle->metadata, 0, sizeof(handle->metadata));
        handle->data      = NULL;
        handle->data_size = 0;
        return handle;
    }

    /* Read first block for header */
    uint8_t* first_block = (uint8_t*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE);
    if (!first_block) { free_mem(handle); return NULL; }

    if (!minimafs_read_blocks(drive, entry.block_offset, 1, first_block)) {
        free_mem(first_block); free_mem(handle); return NULL;
    }

    minimafs_file_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    minimafs_parse_file_header((const char*)first_block, &meta);
    meta.block_offset = entry.block_offset;
    meta.block_count  = entry.block_count;

    const char* data_marker = strstr((const char*)first_block, "@DATA@\n");
    uint32_t data_offset    = data_marker ?
        (uint32_t)(data_marker - (const char*)first_block) + 7 : 0;
    uint32_t data_length    = meta.data_length;

    if (data_length == 0 && meta.file_length > data_offset + 5)
        data_length = meta.file_length - data_offset - 5;
    if (data_offset + data_length > total_size) {
        data_length = (total_size > data_offset) ? total_size - data_offset : 0;
    }

    handle->metadata              = meta;
    handle->data_offset_in_blocks = data_offset;
    handle->data_size             = data_length;

    free_mem(first_block);

#define MINIMAFS_STREAMING_THRESHOLD (2 * 1024 * 1024)

    if (data_length > MINIMAFS_STREAMING_THRESHOLD) {
        handle->use_streaming = true;
        handle->data          = NULL;
        handle->stream_cache  = (uint8_t*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE);
        if (!handle->stream_cache) {
            /* Fall back to full load */
            handle->use_streaming = false;
            goto full_load;
        }
        return handle;
    }

full_load:
    handle->use_streaming = false;
    handle->data          = NULL;

    if (data_length > 0) {
        uint8_t* raw = (uint8_t*)alloc_unzeroed(total_size);
        if (!raw) { free_mem(handle); return NULL; }

        if (!minimafs_read_blocks(drive, entry.block_offset, entry.block_count, raw)) {
            free_mem(raw); free_mem(handle); return NULL;
        }

        handle->data = (uint8_t*)alloc_unzeroed(data_length);
        if (!handle->data) { free_mem(raw); free_mem(handle); return NULL; }
        memcpy(handle->data, raw + data_offset, data_length);
        free_mem(raw);
    }

    return handle;
}

void minimafs_close(minimafs_file_handle_t* handle) {
    if (!handle) return;

    if (handle->modified && !handle->read_only) {
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

                    /* HEAP – was on stack (crash!) */
                    minimafs_folder_desc_t* pd =
                        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
                    if (pd) {
                        if (minimafs_read_folder_desc(drive, parent, pd)) {
                            uint32_t idx = 0;
                            if (minimafs_find_entry_in_folder(pd, filename, &idx)) {
                                pd->entries[idx].block_offset = handle->metadata.block_offset;
                                pd->entries[idx].block_count  = handle->metadata.block_count;
                                pd->entries[idx].hidden       = handle->metadata.hidden;
                                minimafs_write_folder_desc(drive, pd);
                            }
                        }
                        free_mem(pd);
                    }
                }
            }
        }
    }

    if (handle->data)         { free_mem(handle->data);         handle->data         = NULL; }
    if (handle->stream_cache) { free_mem(handle->stream_cache); handle->stream_cache = NULL; }
    handle->open = false;
    free_mem(handle);
}

uint32_t minimafs_read(minimafs_file_handle_t* handle, void* buffer, uint32_t size) {
    if (!handle || !handle->open || !buffer) return 0;
    if (handle->position >= handle->data_size)  return 0;

    uint32_t available = handle->data_size - handle->position;
    uint32_t to_read   = (size < available) ? size : available;

    if (handle->use_streaming) {
        if (!handle->stream_cache) return 0;

        minimafs_drive_t* drive = get_drive(handle->drive_number);
        if (!drive || !drive->mounted) return 0;

        uint32_t bytes_read_total = 0;
        uint32_t remaining        = to_read;

        while (remaining > 0) {
            uint32_t file_offset    = handle->data_offset_in_blocks + handle->position;
            uint32_t block_index    = file_offset / MINIMAFS_BLOCK_SIZE;
            uint32_t offset_in_blk  = file_offset % MINIMAFS_BLOCK_SIZE;

            if (block_index >= handle->file_block_count) break;

            if (block_index != handle->cached_block_index) {
                if (!minimafs_read_blocks(drive,
                        handle->file_block_offset + block_index, 1,
                        handle->stream_cache)) break;
                handle->cached_block_index = block_index;
            }

            uint32_t avail_blk = MINIMAFS_BLOCK_SIZE - offset_in_blk;
            uint32_t left      = handle->data_size - handle->position;
            uint32_t copy      = avail_blk;
            if (copy > remaining) copy = remaining;
            if (copy > left)      copy = left;

            memcpy((uint8_t*)buffer + bytes_read_total,
                   handle->stream_cache + offset_in_blk, copy);

            bytes_read_total  += copy;
            handle->position  += copy;
            remaining         -= copy;
        }
        return bytes_read_total;
    }

    /* Standard (in-memory) mode */
    if (!handle->data) return 0;
    memcpy(buffer, handle->data + handle->position, to_read);
    handle->position += to_read;
    return to_read;
}

uint32_t minimafs_write(minimafs_file_handle_t* handle, const void* buffer, uint32_t size) {
    if (!handle || !handle->open || handle->read_only || !buffer) return 0;

    uint32_t needed = handle->position + size;
    if (needed > handle->data_size) {
        uint8_t* new_data = (uint8_t*)alloc_unzeroed(needed);
        if (!new_data) return 0;
        if (handle->data) { memcpy(new_data, handle->data, handle->data_size); free_mem(handle->data); }
        handle->data      = new_data;
        handle->data_size = needed;
    }

    memcpy(handle->data + handle->position, buffer, size);
    handle->position += size;
    handle->modified  = true;
    minimafs_get_datetime(handle->metadata.last_changed, sizeof(handle->metadata.last_changed));
    handle->metadata.data_length = handle->data_size;
    return size;
}

bool minimafs_seek(minimafs_file_handle_t* handle, uint32_t offset) {
    if (!handle) return false;
    if (offset > handle->data_size) offset = handle->data_size;
    handle->position = offset;
    return true;
}

bool minimafs_delete_file(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return false;

    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* pd =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!pd) return false;

    if (!minimafs_read_folder_desc(drive, parent, pd)) { free_mem(pd); return false; }

    uint32_t idx = 0;
    if (!minimafs_find_entry_in_folder(pd, filename, &idx)) { free_mem(pd); return false; }
    if (pd->entries[idx].type == MINIMAFS_TYPE_DIR)          { free_mem(pd); return false; }

    minimafs_dir_entry_t entry = pd->entries[idx];
    if (entry.block_count > 0)
        block_free_run(drive_num, entry.block_offset, entry.block_count);

    /* Shift remaining entries */
    for (uint32_t i = idx; i + 1 < pd->entry_count; i++)
        pd->entries[i] = pd->entries[i + 1];
    pd->entry_count--;

    minimafs_refresh_storage_desc(drive);
    bool ok = minimafs_write_folder_desc(drive, pd);
    free_mem(pd);
    return ok;
}

/* ================================================================
 * DIRECTORY OPERATIONS  (all folder_desc on heap)
 * ================================================================ */

bool minimafs_mkdir(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return false;

    if (local_path[0]=='\0' || (local_path[0]=='/'&&local_path[1]=='\0')) return true;

    char parent[MINIMAFS_MAX_PATH];
    char dirname[MINIMAFS_MAX_FILENAME];
    const char* last_slash = strrchr(local_path, '/');
    if (last_slash) {
        size_t plen = (size_t)(last_slash - local_path);
        memcpy(parent, local_path, plen);
        parent[plen] = '\0';
        strcpy(dirname, last_slash + 1);
    } else {
        parent[0] = '\0';
        strcpy(dirname, local_path);
    }
    if (dirname[0] == '\0') return false;

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* parent_desc =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!parent_desc) return false;

    if (!minimafs_read_folder_desc(drive, parent, parent_desc)) {
        free_mem(parent_desc); return false;
    }

    uint32_t dir_block = block_alloc(drive_num);
    if (dir_block == 0xFFFFFFFF) {
        serial_write_str("MinimaFS: No free blocks\n");
        free_mem(parent_desc); return false;
    }

    if (parent_desc->entry_count >= 256) {
        block_free_run(drive_num, dir_block, 1);
        free_mem(parent_desc); return false;
    }

    minimafs_dir_entry_t* e = &parent_desc->entries[parent_desc->entry_count++];
    strcpy(e->name, dirname);
    e->type         = MINIMAFS_TYPE_DIR;
    e->block_offset = dir_block;
    e->block_count  = 1;
    e->hidden       = false;

    if (!minimafs_write_folder_desc(drive, parent_desc)) {
        block_free_run(drive_num, dir_block, 1);
        free_mem(parent_desc); return false;
    }
    free_mem(parent_desc);

    /* Write empty folder.desc for the new directory */
    minimafs_folder_desc_t* new_desc =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!new_desc) return false;
    memset(new_desc, 0, sizeof(*new_desc));
    strcpy(new_desc->path, local_path);
    new_desc->block_offset = dir_block;
    new_desc->entry_count  = 0;

    bool ok = minimafs_write_folder_desc(drive, new_desc);
    free_mem(new_desc);
    if (!ok) { block_free_run(drive_num, dir_block, 1); return false; }

    minimafs_refresh_storage_desc(drive);
    return true;
}

bool minimafs_rmdir(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return false;
    if (local_path[0]=='\0' || (local_path[0]=='/'&&local_path[1]=='\0')) return false;

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* target =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!target) return false;

    if (!minimafs_read_folder_desc(drive, local_path, target) ||
        target->entry_count > 0) {
        free_mem(target); return false;
    }
    free_mem(target);

    char dirname[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, dirname);

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* pd =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!pd) return false;

    if (!minimafs_read_folder_desc(drive, parent, pd)) { free_mem(pd); return false; }

    uint32_t idx = 0;
    if (!minimafs_find_entry_in_folder(pd, dirname, &idx) ||
        pd->entries[idx].type != MINIMAFS_TYPE_DIR) {
        free_mem(pd); return false;
    }

    uint32_t to_free = pd->entries[idx].block_offset;
    for (uint32_t i = idx; i + 1 < pd->entry_count; i++)
        pd->entries[i] = pd->entries[i + 1];
    pd->entry_count--;

    bool ok = minimafs_write_folder_desc(drive, pd);
    free_mem(pd);
    if (!ok) return false;

    if (to_free) block_free_run(drive_num, to_free, 1);
    minimafs_refresh_storage_desc(drive);
    return true;
}

uint32_t minimafs_list_dir(const char* path, minimafs_dir_entry_t* entries,
                           uint32_t max_entries) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return 0;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return 0;

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* desc =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!desc) return 0;

    if (!minimafs_read_folder_desc(drive, local_path, desc)) {
        free_mem(desc); return 0;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < desc->entry_count && count < max_entries; i++) {
        if (!desc->entries[i].hidden)
            entries[count++] = desc->entries[i];
    }

    free_mem(desc);
    return count;
}

bool minimafs_exists(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return false;

    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);
    if (filename[0] == '\0') return true;

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* pd =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!pd) return false;

    if (!minimafs_read_folder_desc(drive, parent, pd)) { free_mem(pd); return false; }
    bool found = minimafs_find_entry_in_folder(pd, filename, NULL);
    free_mem(pd);
    return found;
}

bool minimafs_is_dir(const char* path) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return false;

    if (local_path[0]=='\0' || (local_path[0]=='/'&&local_path[1]=='\0')) return true;

    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* pd =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!pd) return false;

    if (!minimafs_read_folder_desc(drive, parent, pd)) { free_mem(pd); return false; }

    uint32_t idx = 0;
    bool found = minimafs_find_entry_in_folder(pd, filename, &idx);
    bool is_dir = found && (pd->entries[idx].type == MINIMAFS_TYPE_DIR);
    free_mem(pd);
    return is_dir;
}

/* ================================================================
 * SELF TEST
 * ================================================================ */

static bool minimafs_self_test_drive(minimafs_drive_t* drive) {
    if (!drive || !drive->device_handle) return false;
    minimafs_disk_device_t* disk = (minimafs_disk_device_t*)drive->device_handle;
    if (!disk->ahci_drive || disk->sector_size == 0) return false;
    if (!ensure_dma_bounce(drive->drive_number)) return false;
    if ((MINIMAFS_BLOCK_SIZE % disk->sector_size) != 0) return false;

    uint8_t*  bounce           = (uint8_t*)g_dma_bounce[drive->drive_number];
    uint32_t  sectors_per_block = MINIMAFS_BLOCK_SIZE / disk->sector_size;
    return ahci_read(disk->ahci_drive, 0, sectors_per_block, bounce);
}

/* ================================================================
 * MOUNT / UNMOUNT / FORMAT
 * ================================================================ */

int minimafs_mount(void* device_handle, uint8_t drive_number) {
    if (drive_number >= MINIMAFS_MAX_DRIVES || !device_handle) return 0;

    minimafs_drive_t* drive = get_drive(drive_number);
    if (drive->mounted) { serial_write_str("MinimaFS: Already mounted\n"); return 2; }

    drive->device_handle = device_handle;

#if MINIMAFS_ENABLE_SELF_TEST
    if (!g_self_test_done[drive_number]) {
        serial_write_str("MinimaFS: Self-test...\n");
        if (!minimafs_self_test_drive(drive)) {
            serial_write_str("MinimaFS: Self-test FAILED\n");
            return 0;
        }
        g_self_test_done[drive_number] = true;
        serial_write_str("MinimaFS: Self-test OK\n");
    }
#endif

    /* Read storage.desc – heap allocated */
    minimafs_storage_desc_t* sd =
        (minimafs_storage_desc_t*)alloc(sizeof(minimafs_storage_desc_t));
    if (!sd) { serial_write_str("MinimaFS: OOM mount\n"); return 0; }
    memset(sd, 0, sizeof(*sd));

    if (!minimafs_read_storage_desc(drive, sd)) {
        serial_write_str("MinimaFS: Failed to parse storage.desc\n");
        free_mem(sd); return 3;
    }

    if (sd->magic != MINIMAFS_MAGIC || sd->root_block == 0 || sd->total_blocks == 0) {
        serial_write_str("MinimaFS: Invalid storage.desc\n");
        free_mem(sd); return 5;
    }

    if (sd->root_block >= sd->total_blocks) {
        serial_write_str("MinimaFS: root_block out of range\n");
        free_mem(sd); return 4;
    }

    drive->storage_desc = *sd;
    free_mem(sd);

    drive->drive_number = drive_number;
    strncpy(drive->drive_name, drive->storage_desc.drive_name,
            sizeof(drive->drive_name) - 1);
    drive->drive_name[sizeof(drive->drive_name) - 1] = '\0';
    if (drive->drive_name[0] == '\0')
        sprintf(drive->drive_name, "%u", drive_number);

    block_alloc_init(drive_number, drive->storage_desc.total_blocks);
    block_alloc_mark_used(drive_number, 0, 1);
    block_alloc_mark_used(drive_number, drive->storage_desc.root_block, 1);

    minimafs_scan_directory(drive, drive->storage_desc.root_block);
    minimafs_refresh_storage_desc(drive);
    minimafs_get_datetime(drive->storage_desc.last_mounted,
                          sizeof(drive->storage_desc.last_mounted));
    minimafs_write_storage_desc(drive);

    drive->mounted = true;
    serial_write_str("MinimaFS: Drive ");
    serial_write_dec(drive_number);
    serial_write_str(" mounted OK\n");
    return 1;
}

bool minimafs_unmount(uint8_t drive_number) {
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive || !drive->mounted) return false;

    minimafs_refresh_storage_desc(drive);
    minimafs_write_storage_desc(drive);

    if (g_dma_bounce[drive_number]) {
        free_pages(g_dma_bounce[drive_number], MINIMAFS_DMA_BOUNCE_BLOCKS);
        g_dma_bounce[drive_number] = NULL;
    }

    drive->mounted        = false;
    drive->device_handle  = NULL;
    return true;
}

bool minimafs_format(void* device_handle, uint64_t size,
                     uint8_t drive_number, const char* drive_name) {
    serial_write_str("MinimaFS: Formatting drive ");
    serial_write_dec(drive_number);
    serial_write_str(" (");
    serial_write_dec((uint32_t)(size / (1024 * 1024)));
    serial_write_str(" MB)\n");

    uint32_t total_blocks = (uint32_t)(size / MINIMAFS_BLOCK_SIZE);
    if (total_blocks > MINIMAFS_BITMAP_SIZE * 8) {
        serial_write_str("MinimaFS: Drive too large for bitmap\n");
        return false;
    }

    block_alloc_init(drive_number, total_blocks);

    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive) return false;
    drive->device_handle = device_handle;
    drive->drive_number  = drive_number;

    /* Heap-allocate storage_desc */
    minimafs_storage_desc_t* storage_desc =
        (minimafs_storage_desc_t*)alloc(sizeof(minimafs_storage_desc_t));
    if (!storage_desc) { serial_write_str("MinimaFS: OOM format\n"); return false; }
    memset(storage_desc, 0, sizeof(*storage_desc));

    storage_desc->magic        = MINIMAFS_MAGIC;
    storage_desc->drive_number = drive_number;

    if (drive_name) {
        strncpy(storage_desc->drive_name, drive_name,
                sizeof(storage_desc->drive_name) - 1);
        storage_desc->drive_name[sizeof(storage_desc->drive_name) - 1] = '\0';
    } else {
        snprintf(storage_desc->drive_name, sizeof(storage_desc->drive_name),
                 "%u", drive_number);
    }

    storage_desc->total_size = size;
    storage_desc->total_blocks = total_blocks;

    uint32_t root_block = block_alloc_run(drive_number, 1);
    if (root_block == 0 || root_block == 0xFFFFFFFF) {
        serial_write_str("MinimaFS: Failed to alloc root block\n");
        free_mem(storage_desc); return false;
    }

    storage_desc->root_block   = root_block;
    block_alloc_mark_used(drive_number, 0, 1);
    block_alloc_mark_used(drive_number, root_block, 1);
    storage_desc->used_blocks  = 2;
    storage_desc->free_blocks  = total_blocks - 2;
    storage_desc->used_size    = (uint64_t)storage_desc->used_blocks * MINIMAFS_BLOCK_SIZE;
    storage_desc->free_size    = size - storage_desc->used_size;

    minimafs_get_datetime(storage_desc->created_date, sizeof(storage_desc->created_date));
    minimafs_get_datetime(storage_desc->last_mounted, sizeof(storage_desc->last_mounted));

    drive->storage_desc = *storage_desc;
    free_mem(storage_desc);

    if (!minimafs_write_storage_desc(drive)) {
        serial_write_str("MinimaFS: Failed to write storage.desc\n");
        return false;
    }

    /* Write root folder.desc */
    char* folder_buf = (char*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE);
    if (!folder_buf) { serial_write_str("MinimaFS: OOM root folder\n"); return false; }
    memset(folder_buf, 0, MINIMAFS_BLOCK_SIZE);
    char* fptr = folder_buf;
    fptr += sprintf(fptr, "FOLDER:/\n");
    fptr += sprintf(fptr, "ENTRIES:0\n");
    fptr += sprintf(fptr, "@END\n");

    bool ok = minimafs_write_blocks(drive, root_block, 1, folder_buf);
    free_mem(folder_buf);

    if (!ok) { serial_write_str("MinimaFS: Failed to write root folder.desc\n"); return false; }

    serial_write_str("MinimaFS: Format complete\n");
    return true;
}

/* ================================================================
 * METADATA
 * ================================================================ */

bool minimafs_get_metadata(const char* path, minimafs_file_metadata_t* metadata) {
    if (!metadata) return false;
    minimafs_file_handle_t* h = minimafs_open(path, true);
    if (!h) return false;
    *metadata = h->metadata;
    minimafs_close(h);
    return true;
}

bool minimafs_set_metadata(const char* path, const minimafs_file_metadata_t* metadata) {
    if (!metadata) return false;
    minimafs_file_handle_t* h = minimafs_open(path, false);
    if (!h) return false;

    minimafs_file_metadata_t updated = h->metadata;
    strcpy(updated.filetype,   metadata->filetype);
    strcpy(updated.fileformat, metadata->fileformat);
    updated.runnable   = metadata->runnable;
    updated.entrypoint = metadata->entrypoint;
    strcpy(updated.run_with, metadata->run_with);
    updated.hidden = metadata->hidden;
    minimafs_get_datetime(updated.last_changed, sizeof(updated.last_changed));

    bool ok = false;
    minimafs_drive_t* drive = get_drive(h->drive_number);
    if (drive && drive->mounted) {
        char local_path[MINIMAFS_MAX_PATH];
        uint8_t dn;
        if (minimafs_parse_path(h->path, &dn, local_path)) {
            ok = minimafs_write_file_to_disk(drive, local_path, &updated,
                                             h->data, h->data_size);
            if (ok) {
                char filename[MINIMAFS_MAX_FILENAME];
                char parent[MINIMAFS_MAX_PATH];
                minimafs_split_local_path(local_path, parent, filename);

                minimafs_folder_desc_t* pd =
                    (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
                if (pd) {
                    if (minimafs_read_folder_desc(drive, parent, pd)) {
                        uint32_t idx = 0;
                        if (minimafs_find_entry_in_folder(pd, filename, &idx)) {
                            pd->entries[idx].hidden = updated.hidden;
                            minimafs_write_folder_desc(drive, pd);
                        }
                    }
                    free_mem(pd);
                }
            }
        }
    }

    minimafs_close(h);
    return ok;
}

bool minimafs_get_storage_desc(uint8_t drive_number, minimafs_storage_desc_t* desc) {
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive || !drive->mounted) return false;
    *desc = drive->storage_desc;
    return true;
}

/* ================================================================
 * WRITE FILE SEGMENTS (public API for audio/large files)
 * ================================================================ */

bool minimafs_write_file_segments(const char* path,
                                  const void* seg1, uint32_t seg1_len,
                                  const void* seg2, uint32_t seg2_len,
                                  const char* filetype, const char* fileformat) {
    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return false;

    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);

    /* HEAP – was on stack (crash!) */
    minimafs_folder_desc_t* pd =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!pd) return false;

    if (!minimafs_read_folder_desc(drive, parent, pd)) {
        free_mem(pd); return false;
    }

    uint32_t idx = 0;
    bool exists = minimafs_find_entry_in_folder(pd, filename, &idx);
    if (exists && pd->entries[idx].type == MINIMAFS_TYPE_DIR) {
        free_mem(pd); return false;
    }

    minimafs_file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    strncpy(metadata.filename,      filename, sizeof(metadata.filename)      - 1);
    strncpy(metadata.filetype,      filetype ? filetype : "binary",
            sizeof(metadata.filetype)      - 1);
    strncpy(metadata.fileformat,    fileformat ? fileformat : "bin",
            sizeof(metadata.fileformat)    - 1);
    strncpy(metadata.parent_folder, parent,   sizeof(metadata.parent_folder) - 1);
    minimafs_get_datetime(metadata.created_date, sizeof(metadata.created_date));
    minimafs_get_datetime(metadata.last_changed, sizeof(metadata.last_changed));

    if (exists) {
        metadata.block_offset = pd->entries[idx].block_offset;
        metadata.block_count  = pd->entries[idx].block_count;
        metadata.hidden       = pd->entries[idx].hidden;
    }

    if (!minimafs_write_file_to_disk_segments(drive, local_path, &metadata,
                                              seg1, seg1_len, seg2, seg2_len)) {
        free_mem(pd); return false;
    }

    if (!exists) {
        if (pd->entry_count >= 256) { free_mem(pd); return false; }
        idx = pd->entry_count++;
        memset(&pd->entries[idx], 0, sizeof(minimafs_dir_entry_t));
        strncpy(pd->entries[idx].name, filename,
                sizeof(pd->entries[idx].name) - 1);
        pd->entries[idx].type = MINIMAFS_TYPE_FILE;
    }
    pd->entries[idx].block_offset = metadata.block_offset;
    pd->entries[idx].block_count  = metadata.block_count;
    pd->entries[idx].hidden       = metadata.hidden;

    bool ok = minimafs_write_folder_desc(drive, pd);
    free_mem(pd);
    return ok;
}

/* ================================================================
 * UTILITY
 * ================================================================ */

uint32_t minimafs_tell(minimafs_file_handle_t* h) { return h ? h->position   : 0; }
uint32_t minimafs_size(minimafs_file_handle_t* h) { return h ? h->data_size  : 0; }
bool     minimafs_eof (minimafs_file_handle_t* h) { return !h || h->position >= h->data_size; }

bool minimafs_read_line(minimafs_file_handle_t* handle, char* buffer,
                        size_t buffer_size) {
    if (!handle || !buffer || buffer_size == 0) return false;
    size_t pos = 0;
    while (pos < buffer_size - 1 && !minimafs_eof(handle)) {
        char ch;
        if (minimafs_read(handle, &ch, 1) != 1) break;
        if (ch == '\n') break;
        if (ch != '\r') buffer[pos++] = ch;
    }
    buffer[pos] = '\0';
    return pos > 0;
}

bool getvalfromsplit(const char* str, const char* delimiter, int index,
                     char* output, size_t output_size) {
    if (!str || !delimiter || !output || index < 1 || output_size == 0) return false;

    size_t str_len = strlen(str);
    char*  temp    = (char*)alloc(str_len + 1);
    if (!temp) return false;
    strcpy(temp, str);

    char*  token    = temp;
    char*  next     = NULL;
    int    current  = 1;
    size_t dlen     = strlen(delimiter);
    bool   found    = false;

    while (token) {
        next = strstr(token, delimiter);
        if (next) { *next = '\0'; next += dlen; }

        if (current == index) {
            strncpy(output, token, output_size - 1);
            output[output_size - 1] = '\0';
            found = true;
            break;
        }
        current++;
        token = next;
    }

    free_mem(temp);
    return found;
}

int32_t minimafs_parse_int(const char* str) {
    if (!str) return 0;
    int32_t r = 0; bool neg = false;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { neg = true; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') r = r * 10 + (*str++ - '0');
    return neg ? -r : r;
}

bool minimafs_get_config_value(const char* key, const char* path,
                               char* output, size_t output_size) {
    if (!key || !path || !output) return false;
    int32_t offset = findinfile(key, path);
    if (offset < 0) return false;

    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) return false;
    minimafs_seek(f, (uint32_t)offset);

    char line[256];
    bool ok = minimafs_read_line(f, line, sizeof(line));
    minimafs_close(f);
    if (!ok) return false;

    return getvalfromsplit(line, "=", 2, output, output_size);
}

int32_t findinfile(const char* needle, const char* path) {
    if (!needle || !path) return -1;

    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) return -1;

    uint32_t needle_len = strlen(needle);
    uint32_t file_size  = minimafs_size(f);

    if (needle_len == 0 || needle_len > file_size) { minimafs_close(f); return -1; }

    const uint32_t CHUNK = 4096;
    uint8_t* buf = (uint8_t*)alloc(CHUNK + needle_len);
    if (!buf) { minimafs_close(f); return -1; }

    uint32_t offset = 0;
    int32_t  result = -1;

    while (offset < file_size) {
        uint32_t to_read = file_size - offset;
        if (to_read > CHUNK) to_read = CHUNK;

        minimafs_seek(f, offset);
        uint32_t got = minimafs_read(f, buf, to_read);
        if (got == 0) break;

        for (uint32_t i = 0; i + needle_len <= got; i++) {
            bool match = true;
            for (uint32_t j = 0; j < needle_len; j++) {
                if (buf[i + j] != (uint8_t)needle[j]) { match = false; break; }
            }
            if (match) { result = (int32_t)(offset + i); goto done; }
        }

        /* Overlap by needle_len to catch cross-chunk matches */
        if (needle_len > 1 && got >= needle_len - 1)
            offset += got - (needle_len - 1);
        else
            offset += got;
    }

done:
    free_mem(buf);
    minimafs_close(f);
    return result;
}

/* ================================================================
 * APPEND FILE  (streaming, heap-safe)
 * ================================================================ */

/* Minimal streaming reader for old file data during append */
typedef struct {
    minimafs_drive_t* drive;
    uint32_t file_block;
    uint32_t file_offset;
    uint32_t data_len;
    uint32_t data_pos;
    uint32_t cached_block;
    uint8_t* block_buf;
} minimafs_old_data_reader_t;

static bool odr_init(minimafs_old_data_reader_t* r, minimafs_drive_t* drive,
                     uint32_t file_block, uint32_t file_offset, uint32_t data_len) {
    if (!r || !drive) return false;
    r->drive        = drive;
    r->file_block   = file_block;
    r->file_offset  = file_offset;
    r->data_len     = data_len;
    r->data_pos     = 0;
    r->cached_block = 0xFFFFFFFF;
    r->block_buf    = (uint8_t*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE);
    return (r->block_buf != NULL);
}

static void odr_free(minimafs_old_data_reader_t* r) {
    if (r && r->block_buf) { free_mem(r->block_buf); r->block_buf = NULL; }
}

static uint32_t odr_read(minimafs_old_data_reader_t* r, uint8_t* dst, uint32_t len) {
    if (!r || !dst || len == 0 || r->data_pos >= r->data_len) return 0;
    uint32_t copied = 0;
    while (len > 0 && r->data_pos < r->data_len) {
        uint32_t fp      = r->file_offset + r->data_pos;
        uint32_t bi      = fp / MINIMAFS_BLOCK_SIZE;
        uint32_t off_blk = fp % MINIMAFS_BLOCK_SIZE;

        if (bi != r->cached_block) {
            if (!minimafs_read_blocks(r->drive, r->file_block + bi, 1, r->block_buf))
                return copied;
            r->cached_block = bi;
        }

        uint32_t avail = MINIMAFS_BLOCK_SIZE - off_blk;
        uint32_t left  = r->data_len - r->data_pos;
        uint32_t cp    = avail < len ? avail : len;
        if (cp > left) cp = left;

        memcpy(dst, r->block_buf + off_blk, cp);
        dst          += cp;
        copied       += cp;
        len          -= cp;
        r->data_pos  += cp;
    }
    return copied;
}

bool minimafs_append_file(const char* path, const void* data, uint32_t data_len) {
    if (!path || !data || data_len == 0) return false;

    uint8_t drive_num;
    char local_path[MINIMAFS_MAX_PATH];
    if (!minimafs_parse_path(path, &drive_num, local_path)) return false;

    minimafs_drive_t* drive = get_drive(drive_num);
    if (!drive || !drive->mounted) return false;

    char filename[MINIMAFS_MAX_FILENAME];
    char parent[MINIMAFS_MAX_PATH];
    minimafs_split_local_path(local_path, parent, filename);

    /* HEAP */
    minimafs_folder_desc_t* pd =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!pd) return false;

    if (!minimafs_read_folder_desc(drive, parent, pd)) { free_mem(pd); return false; }

    uint32_t idx = 0;
    if (!minimafs_find_entry_in_folder(pd, filename, &idx) ||
        pd->entries[idx].type == MINIMAFS_TYPE_DIR) {
        free_mem(pd); return false;
    }

    minimafs_dir_entry_t entry = pd->entries[idx];

    /* Read first block for header info */
    uint8_t* first = (uint8_t*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE + 1);
    if (!first) { free_mem(pd); return false; }

    if (!minimafs_read_blocks(drive, entry.block_offset, 1, first)) {
        free_mem(first); free_mem(pd); return false;
    }
    first[MINIMAFS_BLOCK_SIZE] = '\0';

    minimafs_file_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    if (!minimafs_parse_file_header((char*)first, &meta)) {
        free_mem(first); free_mem(pd); return false;
    }

    const char* marker = strstr((char*)first, "@DATA@\n");
    if (!marker) { free_mem(first); free_mem(pd); return false; }
    uint32_t old_header_size = (uint32_t)(marker - (char*)first) + 7;
    free_mem(first);

    uint32_t footer_size  = 5;
    uint32_t old_file_len = meta.file_length;
    if (old_file_len == 0) old_file_len = entry.block_count * MINIMAFS_BLOCK_SIZE;
    uint32_t old_data_len = (old_file_len > old_header_size + footer_size) ?
                             old_file_len - old_header_size - footer_size : 0;
    uint32_t new_data_len = old_data_len + data_len;

    minimafs_get_datetime(meta.last_changed, sizeof(meta.last_changed));
    meta.data_length  = new_data_len;
    meta.file_length  = 0;

    uint32_t header_size = 0;
    for (int i = 0; i < 2; i++) {
        meta.file_length = header_size + new_data_len + footer_size;
        char* tmp = minimafs_generate_file_header(&meta, &header_size);
        if (!tmp) { free_mem(pd); return false; }
        free_mem(tmp);
    }
    meta.file_length = header_size + new_data_len + footer_size;

    char* header = minimafs_generate_file_header(&meta, &header_size);
    if (!header) { free_mem(pd); return false; }

    uint32_t total_size   = header_size + new_data_len + footer_size;
    uint32_t block_count  = ((total_size + MINIMAFS_BLOCK_SIZE - 1) / MINIMAFS_BLOCK_SIZE);

    uint32_t new_start = block_alloc_run(drive->drive_number, block_count);
    if (new_start == 0xFFFFFFFF) { free_mem(header); free_mem(pd); return false; }

    minimafs_old_data_reader_t reader;
    if (!odr_init(&reader, drive, entry.block_offset, old_header_size, old_data_len)) {
        block_free_run(drive->drive_number, new_start, block_count);
        free_mem(header); free_mem(pd); return false;
    }

    const uint8_t footer[5] = {'@','E','N','D','\n'};

    /* Single block buffer – reused each iteration */
    uint8_t* block_buf = (uint8_t*)alloc_unzeroed(MINIMAFS_BLOCK_SIZE);
    if (!block_buf) {
        odr_free(&reader);
        block_free_run(drive->drive_number, new_start, block_count);
        free_mem(header); free_mem(pd); return false;
    }

    uint32_t seg = 0, seg_pos = 0, data_pos2 = 0;

    for (uint32_t i = 0; i < block_count; i++) {
        memset(block_buf, 0, MINIMAFS_BLOCK_SIZE);
        uint32_t out_pos = 0;

        while (out_pos < MINIMAFS_BLOCK_SIZE && seg < 4) {
            uint32_t rem = MINIMAFS_BLOCK_SIZE - out_pos;

            if (seg == 0) {         /* header */
                uint32_t avail = header_size - seg_pos;
                uint32_t cp = rem < avail ? rem : avail;
                memcpy(block_buf + out_pos, header + seg_pos, cp);
                seg_pos += cp; out_pos += cp;
                if (seg_pos >= header_size) { seg = 1; seg_pos = 0; }

            } else if (seg == 1) {  /* old data */
                if (reader.data_pos >= old_data_len) { seg = 2; seg_pos = 0; continue; }
                uint32_t avail = old_data_len - reader.data_pos;
                uint32_t cp = rem < avail ? rem : avail;
                uint32_t got = odr_read(&reader, block_buf + out_pos, cp);
                out_pos += got;
                if (reader.data_pos >= old_data_len) { seg = 2; seg_pos = 0; }

            } else if (seg == 2) {  /* new data */
                if (data_pos2 >= data_len) { seg = 3; seg_pos = 0; continue; }
                uint32_t avail = data_len - data_pos2;
                uint32_t cp = rem < avail ? rem : avail;
                memcpy(block_buf + out_pos, (const uint8_t*)data + data_pos2, cp);
                data_pos2 += cp; out_pos += cp;
                if (data_pos2 >= data_len) { seg = 3; seg_pos = 0; }

            } else {                /* footer */
                uint32_t avail = 5 - seg_pos;
                uint32_t cp = rem < avail ? rem : avail;
                memcpy(block_buf + out_pos, footer + seg_pos, cp);
                seg_pos += cp; out_pos += cp;
                if (seg_pos >= 5) seg = 4;
            }
        }

        if (!minimafs_write_blocks(drive, new_start + i, 1, block_buf)) {
            free_mem(block_buf); odr_free(&reader);
            block_free_run(drive->drive_number, new_start, block_count);
            free_mem(header); free_mem(pd); return false;
        }
    }

    free_mem(block_buf);
    odr_free(&reader);
    free_mem(header);

    if (entry.block_count > 0)
        block_free_run(drive->drive_number, entry.block_offset, entry.block_count);

    pd->entries[idx].block_offset = new_start;
    pd->entries[idx].block_count  = block_count;

    bool ok = minimafs_write_folder_desc(drive, pd);
    free_mem(pd);
    if (!ok) return false;

    minimafs_refresh_storage_desc(drive);
    serial_write_str("MinimaFS: append complete\n");
    return true;
}
