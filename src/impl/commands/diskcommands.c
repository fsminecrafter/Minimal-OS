#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "minimafshandler.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "systeminfo.h"
#include "music.h"
#include "x86_64/exec_trace.h"
#include "x86_64/safeints.h"
#include "x86_64/pkgformat.h"
#include "x86_64/lzss.h"
#include "fspaths.h"

// ===========================================
// FORMAT COMMAND
// ===========================================

#include "x86_64/storage_setup.h"
#include "x86_64/blockdev.h"

extern const uint8_t _binary_install1_mpkg_start[];
extern const uint8_t _binary_install1_mpkg_end[];
extern const uint8_t _binary_install2_mpkg_start[];
extern const uint8_t _binary_install2_mpkg_end[];
extern const uint8_t _binary_install3_mpkg_start[];
extern const uint8_t _binary_install3_mpkg_end[];

static uint32_t format_pkg_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool format_name_is_safe(const char* name) {
    if (!name || name[0] == '\0') return false;
    if (name[0] == '/') return false;

    const char* p = name;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p == name || p[-1] == '/') &&
            (p[2] == '/' || p[2] == '\0')) {
            return false;
        }
        p++;
    }
    return true;
}

static bool format_mkdir_p(const char* path) {
    if (!path) return false;

    size_t len = strlen(path);
    if (len == 0 || len >= MINIMAFS_MAX_PATH) return false;

    char buf[MINIMAFS_MAX_PATH];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char* colon = strchr(buf, ':');
    if (!colon || colon[1] != '/') return false;
    size_t min_len = (size_t)(colon - buf) + 2;
    if (min_len > len) return false;

    for (size_t i = min_len; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            bool at_end = (buf[i] == '\0');
            char saved = buf[i];
            buf[i] = '\0';
            if (i > min_len) {
                if (!minimafs_is_dir(buf) && !minimafs_mkdir(buf)) {
                    return false;
                }
            }
            buf[i] = saved;
            if (at_end) break;
        }
    }
    return true;
}

static bool format_parent_dir(const char* full_path, char* out, size_t out_size) {
    const char* colon = strchr(full_path, ':');
    if (!colon || colon[1] != '/') return false;
    const char* root_slash = colon + 1;
    const char* last_slash = strrchr(full_path, '/');
    if (!last_slash) return false;

    size_t len = (last_slash == root_slash)
        ? (size_t)(last_slash - full_path) + 1
        : (size_t)(last_slash - full_path);

    if (len >= out_size) return false;
    memcpy(out, full_path, len);
    out[len] = '\0';
    return true;
}

static bool format_install_package(const uint8_t* archive, uint32_t archive_size,
                                   const char* target_dir) {
    if (archive_size < MPKG_HEADER_SIZE ||
        memcmp(archive, MPKG_MAGIC, MPKG_MAGIC_SIZE) != 0 ||
        format_pkg_u32(archive + 8) != MPKG_VERSION) {
        serial_write_str("FORMAT: embedded package is invalid\n");
        return false;
    }

    if (!minimafs_is_dir(target_dir) && !format_mkdir_p(target_dir)) {
        serial_write_str("FORMAT: unable to create target directory ");
        serial_write_str(target_dir);
        serial_write_str("\n");
        return false;
    }

    uint32_t count = format_pkg_u32(archive + 12);
    uint64_t table_end = (uint64_t)MPKG_HEADER_SIZE +
                         (uint64_t)count * MPKG_ENTRY_SIZE;
    if (count == 0 || table_end > archive_size) return false;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = archive + MPKG_HEADER_SIZE + i * MPKG_ENTRY_SIZE;
        char name[MPKG_NAME_SIZE];
        memcpy(name, entry, MPKG_NAME_SIZE);
        name[MPKG_NAME_SIZE - 1] = '\0';
        uint32_t flags = format_pkg_u32(entry + MPKG_NAME_SIZE);
        uint32_t method = format_pkg_u32(entry + MPKG_NAME_SIZE + 4);
        uint32_t raw_size = format_pkg_u32(entry + MPKG_NAME_SIZE + 8);
        uint32_t packed_size = format_pkg_u32(entry + MPKG_NAME_SIZE + 12);
        uint32_t offset = format_pkg_u32(entry + MPKG_NAME_SIZE + 16);

        if (flags & MPKG_FLAG_DIRECTORY) {
            char dir_path[MINIMAFS_MAX_PATH];
            int written = snprintf(dir_path, sizeof(dir_path), "%s/%s", target_dir, name);
            if (written <= 0 || (size_t)written >= sizeof(dir_path)) return false;
            if (!format_name_is_safe(name) || !format_mkdir_p(dir_path)) return false;
            continue;
        }

        if (!format_name_is_safe(name)) return false;
        if ((uint64_t)offset < table_end ||
            (uint64_t)offset + packed_size > archive_size) return false;

        char path[MINIMAFS_MAX_PATH];
        int written = snprintf(path, sizeof(path), "%s/%s", target_dir, name);
        if (written <= 0 || (size_t)written >= sizeof(path)) return false;

        char parent[MINIMAFS_MAX_PATH];
        if (format_parent_dir(path, parent, sizeof(parent)) && !format_mkdir_p(parent)) {
            return false;
        }

        const uint8_t* payload = archive + offset;
        uint8_t* expanded = NULL;
        const uint8_t* data = payload;
        uint32_t data_size = packed_size;
        if (method == MPKG_METHOD_STORE) {
            if (raw_size != packed_size) return false;
        } else if (method == MPKG_METHOD_LZSS) {
            if (raw_size == 0) {
                expanded = NULL;
            } else {
                expanded = (uint8_t*)alloc_unzeroed(raw_size);
                if (!expanded) return false;
            }
            data_size = lzss_decompress(payload, packed_size, expanded, raw_size);
            if (data_size != raw_size) {
                if (expanded) free_mem(expanded);
                return false;
            }
            data = expanded;
        } else return false;

        bool ok = minimafs_write_file_segments(path, data, data_size,
                                               NULL, 0, "binary", "slib");
        if (expanded) free_mem(expanded);
        if (!ok) return false;
        serial_write_str("FORMAT: installed ");
        serial_write_str(name);
        serial_write_str("\n");
    }
    return true;
}

static bool format_install_packages(uint8_t drive_number) {
    char etc[16], programs[16], services[16];
    snprintf(etc, sizeof(etc), "%u:/etc", (unsigned)drive_number);
    snprintf(programs, sizeof(programs), "%u:/programs", (unsigned)drive_number);
    snprintf(services, sizeof(services), "%u:/services", (unsigned)drive_number);
    return format_install_package(_binary_install1_mpkg_start,
                                  (uint32_t)(_binary_install1_mpkg_end - _binary_install1_mpkg_start),
                                  etc) &&
           format_install_package(_binary_install2_mpkg_start,
                                  (uint32_t)(_binary_install2_mpkg_end - _binary_install2_mpkg_start),
                                  programs) &&
           format_install_package(_binary_install3_mpkg_start,
                                  (uint32_t)(_binary_install3_mpkg_end - _binary_install3_mpkg_start),
                                  services);
}

static void setup_say(storage_progress_fn progress, uint32_t percent, const char* text) {
    serial_write_str("setup: ");
    serial_write_str(text);
    serial_write_str("\n");
    if (progress) progress(percent, text);
}

bool storage_format_volume(blockdev_t* device, uint8_t drive_number,
                           const char* label) {
    if (!device) return false;
    if (device->mounted_drive >= 0) {
        serial_write_str("format: unmounting the volume first\n");
        minimafs_unmount((uint8_t)device->mounted_drive);
    }
    minimafs_drive_t* existing = get_drive(drive_number);
    if (existing && existing->mounted) minimafs_unmount(drive_number);

    uint64_t size = blockdev_size_bytes(device);
    char human[24];
    blockdev_format_size(size, human, sizeof(human));
    serial_write_str("format: ");
    serial_write_str(device->name);
    serial_write_str(" ");
    serial_write_str(human);
    serial_write_str("\n");

    if (!minimafs_format(device, size, drive_number, label ? label : "maindisk")) {
        return false;
    }
    if (minimafs_mount(device, drive_number) != 1) return false;
    device->fs = BLOCKDEV_FS_MINIMAFS;
    minimafs_drive_t* drive = get_drive(drive_number);
    if (drive) minimafs_refresh_storage_desc(drive);
    return true;
}

bool storage_populate_system(uint8_t drive_number, storage_progress_fn progress) {
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive || !drive->mounted) return false;

    char path[64];
    setup_say(progress, 70, "Creating system folders");
    const char* dirs[] = { "etc", "services", "programs" };
    for (uint32_t i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%u:/%s", (unsigned)drive_number, dirs[i]);
        if (!minimafs_is_dir(path) && !minimafs_mkdir(path)) {
            serial_write_str("setup: could not create ");
            serial_write_str(path);
            serial_write_str("\n");
            return false;
        }
    }

    snprintf(path, sizeof(path), "%u:/etc/System.conf", (unsigned)drive_number);
    if (!minimafs_exists(path)) minimafs_create_file(path, "text", "conf");

    char date_str[32];
    minimafs_get_datetime(date_str, sizeof(date_str));
    char* conf = (char*)alloc(512);
    if (!conf) return false;
    char* p = conf;
    p += sprintf(p, "# MinimalOS System Configuration\n");
    p += sprintf(p, "# Auto-generated by format\n");
    p += sprintf(p, "#\n");
    p += sprintf(p, "version=%s\n", SYSTEMVER);
    p += sprintf(p, "formatted=%s\n", date_str);
    p += sprintf(p, "hostname=MinimalDevice\n");
    p += sprintf(p, "drive=%u\n", (unsigned int)drive_number);
    p += sprintf(p, "fs=MinimaFS\n");
    p += sprintf(p, "DisplayWidth=1280\n");
    p += sprintf(p, "DisplayHeight=720\n");
    /* Default keyboard layout selector. May be a built-in name (e.g.
     * "US", "SE") or a path to a .kbr file (e.g. "0:/kbr/german.kbr").
     * Change with the 'kbrlayout set <name|path>' terminal command,
     * which keeps this line in sync automatically. */
    p += sprintf(p, "KeyboardLayout=SE\n");
    bool ok = minimafs_write_file_segments(path, conf, (uint32_t)(p - conf),
                                           NULL, 0, "text", "conf");
    free_mem(conf);
    if (!ok) return false;

    setup_say(progress, 80, "Installing system packages");
    if (!format_install_packages(drive_number)) {
        serial_write_str("FORMAT: failed to install resource package\n");
        return false;
    }
    minimafs_refresh_storage_desc(drive);
    return true;
}

static void format_progress_print(uint32_t percent, const char* text) {
    (void)percent;
    graphics_write_textr(text);
    graphics_write_textr("...\n");
}

// format                         the boot volume (or first disk) as 0: + system
// format <device> [drive] [label] [--system]
void cmd_format_debug(int argc, const char** argv) {
    blockdev_t* device = NULL;
    uint8_t drive_number = 0;
    const char* label = "maindisk";
    bool system = false;

    if (argc < 2) {
        initializeminimafs(0);
        device = getminimadrive();
        system = true;
        if (!device) {
            graphics_write_textr("ERROR: No disk found!\n");
            return;
        }
    } else {
        initializeminimafs(0);
        device = blockdev_find(argv[1]);
        if (!device) {
            graphics_write_textr("format: unknown device (see 'disks')\n");
            return;
        }
        int n = storage_free_drive_number(1);
        drive_number = n < 0 ? 1 : (uint8_t)n;
        int pos = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--system") == 0) { system = true; continue; }
            if (pos == 0 && argv[i][0] >= '0' && argv[i][0] <= '9') {
                uint32_t v = 0;
                for (const char* c = argv[i]; *c >= '0' && *c <= '9'; c++) v = v * 10 + (uint32_t)(*c - '0');
                if (v >= MINIMAFS_MAX_DRIVES) {
                    graphics_write_textr("format: drive number must be 0-98\n");
                    return;
                }
                drive_number = (uint8_t)v;
                pos = 1;
            } else {
                label = argv[i];
            }
        }
        if (device->kind == BLOCKDEV_KIND_DISK && device->fs == BLOCKDEV_FS_PARTITIONED) {
            graphics_write_textr("Note: this erases the partition table of ");
            graphics_write_textr(device->name);
            graphics_write_textr(".\n");
        }
        if (drive_number == 0) system = true;
    }

    char human[24];
    blockdev_format_size(blockdev_size_bytes(device), human, sizeof(human));
    graphics_write_textr("Formatting ");
    graphics_write_textr(device->name);
    graphics_write_textr(" (");
    graphics_write_textr(human);
    graphics_write_textr(")...\n");

    if (!storage_format_volume(device, drive_number, label)) {
        graphics_write_textr("ERROR: Formatting failed!\n");
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Mounted as %u:\n", (unsigned)drive_number);
    graphics_write_textr(msg);

    if (system) {
        if (!storage_populate_system(drive_number, format_progress_print)) {
            graphics_write_textr("Failed to install system files.\n");
            return;
        }
    }
    graphics_write_textr("Format complete!\n");
}

// ===========================================
// LISTROOT COMMAND
// ===========================================

void cmd_listroot_debug(int argc, const char** argv) {
    serial_write_str("\n=== LISTROOT DEBUG ===\n");
    
    minimafs_drive_t* drive = get_drive(0);
    if (!drive) {
        serial_write_str("ERROR: get_drive(0) = NULL\n");
        graphics_write_textr("ERROR: Drive 0 not found!\n");
        return;
    }
    
    if (!drive->mounted) {
        serial_write_str("ERROR: Drive 0 not mounted\n");
        graphics_write_textr("ERROR: Drive 0 not mounted! Use 'format' first.\n");
        return;
    }
    
    serial_write_str("Drive 0 is mounted. Root block=");
    serial_write_dec(drive->storage_desc.root_block);
    serial_write_str("\n");
    
    // Heap-allocate - folder_desc_t is ~6KB, unsafe on kernel stack
    minimafs_folder_desc_t* root_desc = (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!root_desc) {
        serial_write_str("ERROR: OOM\n");
        return;
    }

    if (!minimafs_read_folder_desc(drive, "/", root_desc)) {
        serial_write_str("ERROR: Failed to read root folder.desc\n");
        graphics_write_textr("ERROR: Can't read root folder descriptor!\n");
        free_mem(root_desc);
        return;
    }
    
    serial_write_str("Root folder.desc loaded:\n");
    serial_write_str("  Path: ");
    serial_write_str(root_desc->path);
    serial_write_str("\n  Block: ");
    serial_write_dec(root_desc->block_offset);
    serial_write_str("\n  Entry count: ");
    serial_write_dec(root_desc->entry_count);
    serial_write_str("\n");
    
    if (root_desc->entry_count == 0) {
        serial_write_str("Root folder is empty!\n");
        graphics_write_textr("Root directory is empty.\n");
        free_mem(root_desc);
        return;
    }
    
    serial_write_str("All entries (including hidden):\n");
    for (uint32_t i = 0; i < root_desc->entry_count; i++) {
        serial_write_str("  [");
        serial_write_dec(i);
        serial_write_str("] ");
        serial_write_str(root_desc->entries[i].name);
        serial_write_str(" - Block ");
        serial_write_dec(root_desc->entries[i].block_offset);
        serial_write_str(", Type=");
        serial_write_dec(root_desc->entries[i].type);
        serial_write_str(", Hidden=");
        serial_write_dec(root_desc->entries[i].hidden ? 1 : 0);
        serial_write_str("\n");
    }

    free_mem(root_desc);

    // Heap-allocate entries buffer - 64 * ~77 bytes = ~5KB, too large for stack
    minimafs_dir_entry_t* entries = (minimafs_dir_entry_t*)alloc(
        MINIMAFS_MAX_ROOT_ENTRIES * sizeof(minimafs_dir_entry_t));
    if (!entries) {
        serial_write_str("ERROR: OOM for entries\n");
        return;
    }

    uint32_t count = minimafs_list_dir("0:/", entries, MINIMAFS_MAX_ROOT_ENTRIES);
    
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

    free_mem(entries);
    serial_write_str("=== LISTROOT DEBUG DONE ===\n\n");
}

// ===========================================
// RECURSIVE list COMMAND
// ===========================================

// Helper: print a single folder level, then recurse into subdirs
// We copy subdir names before freeing folder, then recurse after
static char g_current_directory[MINIMAFS_MAX_PATH] = "0:/";
static char g_resolved_path[MINIMAFS_MAX_PATH];
static char g_list_local_path[MINIMAFS_MAX_PATH];
static bool fs_append_segment(char* path, size_t* length, const char* segment);
static void fs_remove_last_segment(char* path, size_t* length);

bool fs_resolve_path(const char* input, char* resolved) {
    if (!input || !resolved) return false;

    static char base_local[MINIMAFS_MAX_PATH];
    static char input_local[MINIMAFS_MAX_PATH];
    static char combined[MINIMAFS_MAX_PATH * 2];
    static char normalized[MINIMAFS_MAX_PATH];
    static char segment[MINIMAFS_MAX_FILENAME];
    uint8_t drive_number = 0;

    const char* effective_input = *input ? input : g_current_directory;

    if (strchr(effective_input, ':')) {
        if (!minimafs_parse_path(effective_input, &drive_number, input_local)) return false;
        combined[0] = '\0';
        strncpy(combined, input_local, sizeof(combined) - 1);
    } else {
        if (!minimafs_parse_path(g_current_directory, &drive_number, base_local)) return false;
        if (effective_input[0] == '/') {
            snprintf(combined, sizeof(combined), "%s", effective_input);
        } else {
            snprintf(combined, sizeof(combined), "%s/%s", base_local, effective_input);
        }
    }

    normalized[0] = '/';
    normalized[1] = '\0';
    size_t normalized_length = 1;
    char* cursor = combined;
    while (*cursor == '/') cursor++;

    while (*cursor) {
        size_t segment_length = 0;
        while (*cursor && *cursor != '/') {
            if (segment_length + 1 >= sizeof(segment)) return false;
            segment[segment_length++] = *cursor++;
        }
        segment[segment_length] = '\0';
        while (*cursor == '/') cursor++;

        if (segment_length == 0 || strcmp(segment, ".") == 0) continue;
        if (strcmp(segment, "..") == 0) {
            fs_remove_last_segment(normalized, &normalized_length);
        } else if (!fs_append_segment(normalized, &normalized_length, segment)) {
            return false;
        }
    }

    snprintf(resolved, MINIMAFS_MAX_PATH, "%u:%s", drive_number, normalized);
    return true;
}

const char* fs_get_current_directory(void) {
    return g_current_directory;
}

void list_recursive(uint8_t drive_number, const char* path, int indent) {
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive || !drive->mounted) return;

    // Heap-allocate: this is recursive so each level would add ~6KB to stack
    minimafs_folder_desc_t* folder = (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!folder) return;

    if (!minimafs_read_folder_desc(drive, path, folder)) {
        free_mem(folder);
        return;
    }

    for (int i = 0; i < indent; i++) graphics_write_textr("  ");
    graphics_write_textr(path);
    graphics_write_textr("/\n");

    uint32_t entry_count = folder->entry_count;

    // HEAP — 64 * 1024 bytes is 16x the 4KB kernel stack; never on-stack,
    // especially in a recursive function.
    char (*subdirs)[MINIMAFS_MAX_PATH] =
    (char (*)[MINIMAFS_MAX_PATH])alloc(MINIMAFS_MAX_ROOT_ENTRIES * MINIMAFS_MAX_PATH);
    uint32_t subdir_count = 0;

    if (!subdirs) {
        serial_write_str("list_recursive: OOM allocating subdirs buffer\n");
        free_mem(folder);
        return;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        minimafs_dir_entry_t* entry = &folder->entries[i];

        for (int j = 0; j < indent + 1; j++) graphics_write_textr("  ");
        graphics_write_textr(entry->name);

        if (entry->type == MINIMAFS_TYPE_DIR) {
            graphics_write_textr(" [DIR]\n");
            if (subdir_count < MINIMAFS_MAX_ROOT_ENTRIES) {
                if (strcmp(path, "/") == 0) {
                    snprintf(subdirs[subdir_count], MINIMAFS_MAX_PATH, "/%s", entry->name);
                } else {
                    snprintf(subdirs[subdir_count], MINIMAFS_MAX_PATH, "%s/%s", path, entry->name);
                }
                subdir_count++;
            }
        } else {
            graphics_write_textr("\n");
        }
    }

    free_mem(folder);

    // Recurse into subdirs with folder already freed
    for (uint32_t i = 0; i < subdir_count; i++) {
        list_recursive(drive_number, subdirs[i], indent + 1);
    }

    free_mem(subdirs);
}

void cmd_list(int argc, const char** argv) {
    serial_write_str("\n=== list ===\n");

    const char* requested_path = "";
    bool recursive = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0 ||
            strcmp(argv[i], "--recurvesive") == 0) {
            recursive = true;
        } else if (*requested_path == '\0') {
            requested_path = argv[i];
        }
    }
    if (!fs_resolve_path(requested_path, g_resolved_path)) {
        graphics_write_textr("list: invalid path\n");
        return;
    }

    uint8_t drive_number;
    if (!minimafs_parse_path(g_resolved_path, &drive_number, g_list_local_path)) {
        graphics_write_textr("list: invalid path\n");
        return;
    }

    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive || !drive->mounted) {
        serial_write_str("ERROR: Drive not mounted!\n");
        graphics_write_textr("Drive not mounted!\n");
        return;
    }

    if (!minimafs_is_dir(g_resolved_path)) {
        graphics_write_textr("list: not a directory\n");
        return;
    }

    if (recursive) {
        list_recursive(drive_number, g_list_local_path, 0);
        serial_write_str("=== list DONE ===\n\n");
        return;
    }

    minimafs_dir_entry_t* entries = (minimafs_dir_entry_t*)alloc(
        MINIMAFS_MAX_ROOT_ENTRIES * sizeof(minimafs_dir_entry_t));
    if (!entries) {
        graphics_write_textr("list: out of memory\n");
        return;
    }

    uint32_t count = minimafs_list_dir(g_resolved_path, entries,
                                       MINIMAFS_MAX_ROOT_ENTRIES);
    for (uint32_t i = 0; i < count; i++) {
        graphics_write_textr(entries[i].name);
        if (entries[i].type == MINIMAFS_TYPE_DIR)
            graphics_write_textr(" [DIR]");
        graphics_write_textr("\n");
    }
    free_mem(entries);

    serial_write_str("=== list DONE ===\n\n");
}

static uint8_t parse_u8(const char* s, uint8_t def) {
    if (!s || !*s) return def;
    uint32_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = (val * 10) + (uint32_t)(*s - '0');
        s++;
    }
    if (val > 255) val = 255;
    return (uint8_t)val;
}

static bool fs_append_segment(char* path, size_t* length, const char* segment) {
    size_t segment_length = strlen(segment);
    if (*length + segment_length + 2 >= MINIMAFS_MAX_PATH) return false;
    if (*length > 1) path[(*length)++] = '/';
    memcpy(path + *length, segment, segment_length);
    *length += segment_length;
    path[*length] = '\0';
    return true;
}

static void fs_remove_last_segment(char* path, size_t* length) {
    if (*length <= 1) {
        *length = 1;
        path[1] = '\0';
        return;
    }
    while (*length > 1 && path[*length - 1] != '/') (*length)--;
    if (*length > 1) (*length)--;
    path[*length] = '\0';
}

static bool fs_set_directory_hidden(const char* path, bool hidden) {
    static char local_path[MINIMAFS_MAX_PATH];
    static char parent[MINIMAFS_MAX_PATH];
    static char dirname[MINIMAFS_MAX_FILENAME];
    uint8_t drive_number;

    if (!minimafs_parse_path(path, &drive_number, local_path)) return false;
    minimafs_drive_t* drive = get_drive(drive_number);
    if (!drive) return false;

    const char* slash = strrchr(local_path, '/');
    if (!slash) {
        parent[0] = '\0';
        strncpy(dirname, local_path, sizeof(dirname) - 1);
    } else {
        size_t parent_length = (size_t)(slash - local_path);
        if (parent_length >= sizeof(parent)) return false;
        memcpy(parent, local_path, parent_length);
        parent[parent_length] = '\0';
        strncpy(dirname, slash + 1, sizeof(dirname) - 1);
    }
    dirname[sizeof(dirname) - 1] = '\0';

    minimafs_folder_desc_t* descriptor =
        (minimafs_folder_desc_t*)alloc(sizeof(minimafs_folder_desc_t));
    if (!descriptor || !minimafs_read_folder_desc(drive, parent, descriptor)) {
        if (descriptor) free_mem(descriptor);
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < descriptor->entry_count; i++) {
        if (strcmp(descriptor->entries[i].name, dirname) == 0 &&
            descriptor->entries[i].type == MINIMAFS_TYPE_DIR) {
            descriptor->entries[i].hidden = hidden;
            found = true;
            break;
        }
    }

    bool result = found && minimafs_write_folder_desc(drive, descriptor);
    free_mem(descriptor);
    return result;
}

void cmd_cd(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr(g_current_directory);
        graphics_write_textr("\n");
        return;
    }

    if (!fs_resolve_path(argv[1], g_resolved_path) || !minimafs_is_dir(g_resolved_path)) {
        graphics_write_textr("cd: directory not found: ");
        graphics_write_textr(argv[1]);
        graphics_write_textr("\n");
        return;
    }

    strncpy(g_current_directory, g_resolved_path, sizeof(g_current_directory) - 1);
    g_current_directory[sizeof(g_current_directory) - 1] = '\0';
    graphics_write_textr(g_current_directory);
    graphics_write_textr("\n");
}

void cmd_mdr(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: mdr <directory> [--hidden]\n");
        return;
    }

    bool hidden = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--hidden") == 0) hidden = true;
    }

    if (!fs_resolve_path(argv[1], g_resolved_path) || !minimafs_mkdir(g_resolved_path)) {
        graphics_write_textr("mdr: could not create directory\n");
        return;
    }
    if (hidden && !fs_set_directory_hidden(g_resolved_path, true)) {
        graphics_write_textr("mdr: directory created but hidden flag failed\n");
        return;
    }
    graphics_write_textr("Directory created: ");
    graphics_write_textr(g_resolved_path);
    graphics_write_textr("\n");
}

void cmd_insert(int argc, const char** argv) {
    if (argc < 3) {
        graphics_write_textr("Usage: insert <file> \"text\"\n");
        return;
    }

    if (!fs_resolve_path(argv[1], g_resolved_path) || minimafs_is_dir(g_resolved_path)) {
        graphics_write_textr("insert: invalid file path\n");
        return;
    }

    minimafs_file_handle_t* existing = minimafs_open(g_resolved_path, true);
    if (existing) {
        minimafs_close(existing);
        if (!minimafs_delete_file(g_resolved_path)) {
            graphics_write_textr("insert: could not replace file\n");
            return;
        }
    }
    if (!minimafs_create_file(g_resolved_path, "text", "txt")) {
        graphics_write_textr("insert: could not create file\n");
        return;
    }

    minimafs_file_handle_t* file = minimafs_open(g_resolved_path, false);
    if (!file || minimafs_write(file, argv[2], (uint32_t)strlen(argv[2])) != strlen(argv[2])) {
        if (file) minimafs_close(file);
        graphics_write_textr("insert: write failed\n");
        return;
    }
    minimafs_close(file);
    graphics_write_textr("Inserted text into ");
    graphics_write_textr(g_resolved_path);
    graphics_write_textr("\n");
}

void register_fs_navigation_commands(void) {
    command_register("cd", cmd_cd);
    command_register("mdr", cmd_mdr);
    command_register("insert", cmd_insert);
}

REGISTER_COMMAND(register_fs_navigation_commands);

void register_list(void) {
    command_register("list", cmd_list);
    command_register("ls", cmd_list);
}

void register_format_debug(void) {
    command_register("format", cmd_format_debug);
}

void register_listroot_debug(void) {
    command_register("listroot", cmd_listroot_debug);
    command_register("lsroot", cmd_listroot_debug);
}


// ===========================================
// REMOVE (rm) COMMAND
// ===========================================
//
// Usage: remove <path> [-r|--recursive]
//        rm     <path> [-r|--recursive]
//
// Plain files, and empty directories, are removed directly. A
// non-empty directory requires -r/--recursive, which walks the tree
// depth-first (children before the directory itself, since
// minimafs_rmdir() refuses to remove a non-empty folder.desc) and
// deletes everything underneath before removing the directory.

// Recursively removes `path` (a fully-resolved "N:/..." path) and
// everything under it. Heap-allocates its working buffers (dir
// entries + child-path buffer) rather than using the stack, since this
// function recurses with directory depth and the kernel stack is only
// 4KB per process - same pattern used by list_recursive() and
// minimafs_scan_directory() elsewhere in this codebase.
static bool remove_recursive(const char* path) {
    if (!minimafs_is_dir(path)) {
        return minimafs_delete_file(path);
    }

    minimafs_dir_entry_t* entries = (minimafs_dir_entry_t*)alloc(
        MINIMAFS_MAX_ROOT_ENTRIES * sizeof(minimafs_dir_entry_t));
    if (!entries) {
        serial_write_str("remove_recursive: OOM allocating entries\n");
        return false;
    }

    uint32_t count = minimafs_list_dir(path, entries, MINIMAFS_MAX_ROOT_ENTRIES);

    char* child_path = (char*)alloc_unzeroed(MINIMAFS_MAX_PATH);
    if (!child_path) {
        serial_write_str("remove_recursive: OOM allocating child_path\n");
        free_mem(entries);
        return false;
    }

    bool ok = true;
    size_t path_len = strlen(path);

    for (uint32_t i = 0; i < count && ok; i++) {
        // `path` may or may not already end in '/' (root is "N:/",
        // everything else is "N:/foo" with no trailing slash) - handle
        // both without producing a double slash.
        if (path_len > 0 && path[path_len - 1] == '/') {
            snprintf(child_path, MINIMAFS_MAX_PATH, "%s%s", path, entries[i].name);
        } else {
            snprintf(child_path, MINIMAFS_MAX_PATH, "%s/%s", path, entries[i].name);
        }

        if (!remove_recursive(child_path)) {
            serial_write_str("remove_recursive: failed to remove ");
            serial_write_str(child_path);
            serial_write_str("\n");
            ok = false;
        }
    }

    free_mem(child_path);
    free_mem(entries);

    if (!ok) return false;

    return minimafs_rmdir(path);
}

void cmd_remove(int argc, const char** argv) {
    bool recursive = false;
    const char* requested_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--recursive") == 0) {
            recursive = true;
        } else if (!requested_path) {
            requested_path = argv[i];
        }
    }

    if (!requested_path) {
        graphics_write_textr("Usage: remove <path> [-r|--recursive]\n");
        return;
    }

    if (!fs_resolve_path(requested_path, g_resolved_path)) {
        graphics_write_textr("remove: invalid path\n");
        return;
    }

    // Refuse to touch a drive's root. Resolved paths look like
    // "N:/..." - root is exactly "N:/" (empty local path). Recursing
    // into this would delete the entire drive's contents; minimafs_rmdir()
    // already refuses to remove the root folder.desc itself, but by the
    // time we got there in a recursive delete everything else would
    // already be gone, which is not something to do silently.
    const char* colon = strchr(g_resolved_path, ':');
    if (colon && colon[1] == '/' && colon[2] == '\0') {
        graphics_write_textr("remove: refusing to remove drive root\n");
        return;
    }

    if (!minimafs_exists(g_resolved_path)) {
        graphics_write_textr("remove: no such file or directory: ");
        graphics_write_textr(requested_path);
        graphics_write_textr("\n");
        return;
    }

    bool ok;
    if (minimafs_is_dir(g_resolved_path)) {
        if (recursive) {
            ok = remove_recursive(g_resolved_path);
        } else {
            ok = minimafs_rmdir(g_resolved_path);
            if (!ok) {
                graphics_write_textr("remove: directory not empty (use -r to remove recursively)\n");
                return;
            }
        }
    } else {
        ok = minimafs_delete_file(g_resolved_path);
    }

    if (ok) {
        graphics_write_textr("Removed: ");
        graphics_write_textr(requested_path);
        graphics_write_textr("\n");
    } else {
        graphics_write_textr("remove: failed to remove ");
        graphics_write_textr(requested_path);
        graphics_write_textr("\n");
    }
}

// ===========================================
// CREATE MUSIC FILE COMMAND
// Usage: createmusicfile [drive]
// Writes embedded music.o data to <drive>:/music.adi
// ===========================================

static bool write_music_adi(uint8_t drive_number) {
    minimafs_drive_t* d = get_drive(drive_number);
    if (!d || !d->mounted) {
        graphics_write_textr("Invalid or unmounted drive\n");
        return false;
    }

    const uint8_t* data     = _binary_music_wav_start;
    uint32_t       data_len = (uint32_t)(_binary_music_wav_end - _binary_music_wav_start);
    if (data_len == 0) {
        graphics_write_textr("No embedded music data\n");
        return false;
    }

    const uint32_t sample_rate    = 48000;
    const uint8_t  channels       = 2;
    const uint8_t  volume         = 80;

    // IMA-ADPCM: 2 compressed bytes -> 4 stereo samples (L+R+L+R)
    // So total PCM samples = data_len * 2
    // Length in seconds = (data_len * 2) / (sample_rate * channels)
    uint32_t length_seconds = (data_len * 2) / (sample_rate * channels);
    if (length_seconds == 0) length_seconds = 1;

    char path[32];
    snprintf(path, sizeof(path), "%u:/music.adi", drive_number);

    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "#Audio data generated by ADI converter\n"
        "AudioFormat=IADPCM\n"
        "AudioLength=%u\n"
        "AudioDatalen=%u\n"
        "Globalvol=%u\n"
        "SampleRate=%u\n"
        "Channels=%u\n"
        "#DATA\n",
        length_seconds, data_len, volume, sample_rate, channels
    );
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        graphics_write_textr("Failed to build ADI header\n");
        return false;
    }

    serial_write_str("createmusicfile: writing ");
    serial_write_dec(data_len);
    serial_write_str(" bytes to ");
    serial_write_str(path);
    serial_write_str("\n");

    if (!minimafs_write_file_segments(path,
            header,  (uint32_t)header_len,
            data,    data_len,
            "audio", "adi")) {
        graphics_write_textr("Failed to write music.adi\n");
        return false;
    }

    serial_write_str("createmusicfile: write complete\n");
    graphics_write_textr("music.adi written to ");
    graphics_write_textr(path);
    graphics_write_textr("\n");
    return true;
}

void cmd_createmusicfile(int argc, const char** argv) {
    TRACE_ENTER();
    uint8_t drive = 0;
    if (argc >= 2) drive = parse_u8(argv[1], 0);
    if (!write_music_adi(drive)) {
        graphics_write_textr("createmusicfile failed\n");
    } else {
        graphics_write_textr("Done music file created.\n");
    }
    TRACE_EXIT();
}

void register_createmusicfile(void) {
    command_register("importmusic", cmd_createmusicfile);
}

REGISTER_COMMAND(register_createmusicfile);

// ===========================================
// READ COMMAND
// Usage: read <path> [--metadata]
// Reads a file's data and prints it to the terminal.
// With --metadata also prints the file's header fields.
// ===========================================

void cmd_read(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: read <path> [--metadata]\n");
        graphics_write_textr("  Example: read 0:/etc/system.conf\n");
        graphics_write_textr("  Example: read 0:/etc/system.conf --metadata\n");
        return;
    }

    if (!fs_resolve_path(argv[1], g_resolved_path) ||
        minimafs_is_dir(g_resolved_path)) {
        graphics_write_textr("read: path is not a file\n");
        return;
    }

    const char* path = g_resolved_path;
    bool show_metadata = (argc >= 3 && strcmp(argv[2], "--metadata") == 0);

    serial_write_str("read: opening '");
    serial_write_str(path);
    serial_write_str("'\n");

    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) {
        graphics_write_textr("ERROR: Could not open '");
        graphics_write_textr(path);
        graphics_write_textr("'\n");
        graphics_write_textr("  (File may not exist, or path is a directory)\n");
        serial_write_str("read: minimafs_open returned NULL\n");
        return;
    }

    // ---- Metadata ----
    if (show_metadata) {
        graphics_write_textr("--- Metadata ---\n");

        graphics_write_textr("Name:     ");
        graphics_write_textr(f->metadata.filename[0] ? f->metadata.filename : "(none)");
        graphics_write_textr("\n");

        graphics_write_textr("Type:     ");
        graphics_write_textr(f->metadata.filetype[0] ? f->metadata.filetype : "(none)");
        graphics_write_textr("\n");

        graphics_write_textr("Format:   ");
        graphics_write_textr(f->metadata.fileformat[0] ? f->metadata.fileformat : "(none)");
        graphics_write_textr("\n");

        graphics_write_textr("Parent:   ");
        graphics_write_textr(f->metadata.parent_folder[0] ? f->metadata.parent_folder : "/");
        graphics_write_textr("\n");

        graphics_write_textr("Created:  ");
        graphics_write_textr(f->metadata.created_date[0] ? f->metadata.created_date : "unknown");
        graphics_write_textr("\n");

        graphics_write_textr("Modified: ");
        graphics_write_textr(f->metadata.last_changed[0] ? f->metadata.last_changed : "unknown");
        graphics_write_textr("\n");

        graphics_write_textr("Hidden:   ");
        graphics_write_textr(f->metadata.hidden ? "yes" : "no");
        graphics_write_textr("\n");

        graphics_write_textr("Runnable: ");
        graphics_write_textr(f->metadata.runnable ? "yes" : "no");
        graphics_write_textr("\n");

        // Print sizes without %llu — split into KB + remainder bytes
        uint32_t file_len = f->metadata.file_length;
        uint32_t data_len = f->metadata.data_length;

        graphics_write_textr("File len: ");
        graphics_write_textr_udec(file_len);
        graphics_write_textr(" bytes\n");

        graphics_write_textr("Data len: ");
        graphics_write_textr_udec(data_len);
        graphics_write_textr(" bytes\n");

        graphics_write_textr("Block:    ");
        graphics_write_textr_udec(f->metadata.block_offset);
        graphics_write_textr(" (count: ");
        graphics_write_textr_udec(f->metadata.block_count);
        graphics_write_textr(")\n");

        graphics_write_textr("--- Data ---\n");
    }

    // ---- File data ----
    if (f->data_size == 0) {
        graphics_write_textr("(file has no data content)\n");
        minimafs_close(f);
        return;
    }

    // Read in chunks so we never need a large stack buffer.
    // 256 bytes per chunk keeps stack usage negligible.
    #define READ_CHUNK 256
    uint8_t* chunk = (uint8_t*)alloc_unzeroed(READ_CHUNK + 1);
    if (!chunk) {
        graphics_write_textr("ERROR: OOM reading file\n");
        minimafs_close(f);
        return;
    }

    uint32_t total_read = 0;
    bool last_was_newline = false;

    while (total_read < f->data_size) {
        uint32_t n = minimafs_read(f, chunk, READ_CHUNK);
        if (n == 0) break;

        chunk[n] = '\0';

        // Print byte-by-byte so control characters are handled cleanly.
        // Skip null bytes (padding at end of block).
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            if (c == 0) break;           // end of actual text content
            if (c == '\r') continue;     // strip carriage returns
            last_was_newline = (c == '\n');
            graphics_write_textr_char((char)c);
        }

        total_read += n;
    }

    // Ensure output ends on a fresh line
    if (!last_was_newline) {
        graphics_write_textr("\n");
    }

    free_mem(chunk);
    minimafs_close(f);

    serial_write_str("read: done, ");
    serial_write_dec(total_read);
    serial_write_str(" bytes read\n");
}

void register_read(void) {
    command_register("read", cmd_read);
    command_register("rd", cmd_read);
}

void register_remove(void) {
    command_register("remove", cmd_remove);
    command_register("rm", cmd_remove);
}

REGISTER_COMMAND(register_remove);
REGISTER_COMMAND(register_format_debug);
REGISTER_COMMAND(register_listroot_debug);
REGISTER_COMMAND(register_list);
REGISTER_COMMAND(register_read);
