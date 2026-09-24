#include "x86_64/pkglib.h"
#include "x86_64/pkgformat.h"
#include "x86_64/lzss.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "string.h"

/* Above this raw file size, pkglib_zip() silently falls back to STORE
 * even if lzss was requested - see the identical cap and rationale in
 * commands/pkgcommand.c. */
#define PKGLIB_ZIP_LZSS_MAX_BYTES (256u * 1024u)

static uint32_t pkg_read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t pkg_write_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
    return 4;
}

typedef struct {
    char name[MPKG_NAME_SIZE];
    uint32_t flags;
    uint32_t method;
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint32_t data_offset;
} pkg_entry_t;

static void pkg_parse_entry(const uint8_t* raw, pkg_entry_t* out) {
    memcpy(out->name, raw, MPKG_NAME_SIZE);
    out->name[MPKG_NAME_SIZE - 1] = '\0';

    const uint8_t* p = raw + MPKG_NAME_SIZE;
    out->flags             = pkg_read_u32(p + 0);
    out->method            = pkg_read_u32(p + 4);
    out->uncompressed_size = pkg_read_u32(p + 8);
    out->compressed_size   = pkg_read_u32(p + 12);
    out->data_offset       = pkg_read_u32(p + 16);
}

static bool pkg_name_is_safe(const char* name) {
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

static bool pkg_mkdir_p(const char* path) {
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
                if (!minimafs_is_dir(buf) && !minimafs_mkdir(buf)) return false;
            }
            buf[i] = saved;
            if (at_end) break;
        }
    }
    return true;
}

static bool pkg_parent_dir(const char* full_path, char* out, size_t out_size) {
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

static const char* pkg_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool pkg_extract_entry(const pkg_entry_t* entry, const uint8_t* archive,
                              uint32_t archive_size, const char* target_dir) {
    if (!pkg_name_is_safe(entry->name)) {
        serial_write_str("pkglib: unsafe entry name, skipping: ");
        serial_write_str(entry->name[0] ? entry->name : "(empty)");
        serial_write_str("\n");
        return false;
    }

    char full_path[MINIMAFS_MAX_PATH];
    int written = snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, entry->name);
    if (written <= 0 || (size_t)written >= sizeof(full_path)) {
        serial_write_str("pkglib: path too long: ");
        serial_write_str(entry->name);
        serial_write_str("\n");
        return false;
    }

    if (entry->flags & MPKG_FLAG_DIRECTORY) {
        if (!pkg_mkdir_p(full_path)) {
            serial_write_str("pkglib: failed to create directory ");
            serial_write_str(full_path);
            serial_write_str("\n");
            return false;
        }
        return true;
    }

    char parent[MINIMAFS_MAX_PATH];
    if (pkg_parent_dir(full_path, parent, sizeof(parent))) {
        if (!pkg_mkdir_p(parent)) {
            serial_write_str("pkglib: failed to create parent dir for ");
            serial_write_str(entry->name);
            serial_write_str("\n");
            return false;
        }
    }

    if (entry->compressed_size > 0) {
        uint64_t end = (uint64_t)entry->data_offset + (uint64_t)entry->compressed_size;
        if (entry->data_offset >= archive_size || end > archive_size) {
            serial_write_str("pkglib: corrupt archive (bad data range) for ");
            serial_write_str(entry->name);
            serial_write_str("\n");
            return false;
        }
    }

    const uint8_t* payload = archive + entry->data_offset;
    bool ok;

    if (entry->method == MPKG_METHOD_STORE) {
        if (entry->uncompressed_size != entry->compressed_size) {
            serial_write_str("pkglib: corrupt archive (size mismatch) for ");
            serial_write_str(entry->name);
            serial_write_str("\n");
            return false;
        }
        ok = minimafs_write_file_segments(full_path, payload, entry->compressed_size,
                                          NULL, 0, "binary", "bin");
    } else if (entry->method == MPKG_METHOD_LZSS) {
        uint8_t* decompressed = entry->uncompressed_size
            ? (uint8_t*)alloc_unzeroed(entry->uncompressed_size)
            : NULL;
        if (entry->uncompressed_size && !decompressed) {
            serial_write_str("pkglib: OOM decompressing ");
            serial_write_str(entry->name);
            serial_write_str("\n");
            return false;
        }

        uint32_t produced = entry->uncompressed_size
            ? lzss_decompress(payload, entry->compressed_size,
                              decompressed, entry->uncompressed_size)
            : 0;

        if (entry->uncompressed_size && produced != entry->uncompressed_size) {
            serial_write_str("pkglib: decompression failed for ");
            serial_write_str(entry->name);
            serial_write_str("\n");
            if (decompressed) free_mem(decompressed);
            return false;
        }

        ok = minimafs_write_file_segments(full_path, decompressed, entry->uncompressed_size,
                                          NULL, 0, "binary", "bin");
        if (decompressed) free_mem(decompressed);
    } else {
        serial_write_str("pkglib: unknown compression method for ");
        serial_write_str(entry->name);
        serial_write_str("\n");
        return false;
    }

    if (!ok) {
        serial_write_str("pkglib: failed to write ");
        serial_write_str(full_path);
        serial_write_str("\n");
    }
    return ok;
}

static uint8_t* pkg_load_archive(const char* path, uint32_t* out_size) {
    minimafs_file_handle_t* file = minimafs_open(path, true);
    if (!file) {
        serial_write_str("pkglib: cannot open ");
        serial_write_str(path);
        serial_write_str("\n");
        return NULL;
    }

    uint32_t size = minimafs_size(file);
    if (size < MPKG_HEADER_SIZE) {
        serial_write_str("pkglib: file too small to be a package\n");
        minimafs_close(file);
        return NULL;
    }

    uint8_t* buffer = (uint8_t*)alloc_unzeroed(size);
    if (!buffer) {
        serial_write_str("pkglib: out of memory reading archive\n");
        minimafs_close(file);
        return NULL;
    }

    uint32_t got = minimafs_read(file, buffer, size);
    minimafs_close(file);

    if (got != size) {
        serial_write_str("pkglib: short read on archive\n");
        free_mem(buffer);
        return NULL;
    }

    if (memcmp(buffer, MPKG_MAGIC, MPKG_MAGIC_SIZE) != 0) {
        serial_write_str("pkglib: not a valid .mpkg file (bad magic)\n");
        free_mem(buffer);
        return NULL;
    }

    uint32_t version = pkg_read_u32(buffer + 8);
    if (version != MPKG_VERSION) {
        serial_write_str("pkglib: unsupported package version\n");
        free_mem(buffer);
        return NULL;
    }

    *out_size = size;
    return buffer;
}

static uint32_t pkg_validate_entry_table(const uint8_t* archive, uint32_t archive_size) {
    uint32_t entry_count = pkg_read_u32(archive + 12);
    uint64_t table_bytes = (uint64_t)entry_count * MPKG_ENTRY_SIZE;

    if (entry_count == 0 || table_bytes > (uint64_t)archive_size - MPKG_HEADER_SIZE) {
        serial_write_str("pkglib: corrupt archive (bad entry table)\n");
        return 0;
    }
    return entry_count;
}

/* ============================================================
 * UNZIP / INFO
 * ============================================================ */

bool pkglib_unzip(const char* archive_path, const char* target_dir,
                  uint32_t* out_installed, uint32_t* out_failed) {
    if (out_installed) *out_installed = 0;
    if (out_failed) *out_failed = 0;
    if (!archive_path || !target_dir) return false;

    uint32_t archive_size = 0;
    uint8_t* archive = pkg_load_archive(archive_path, &archive_size);
    if (!archive) return false;

    uint32_t entry_count = pkg_validate_entry_table(archive, archive_size);
    if (entry_count == 0) {
        free_mem(archive);
        return false;
    }

    if (!minimafs_is_dir(target_dir) && !pkg_mkdir_p(target_dir)) {
        serial_write_str("pkglib: could not create target directory\n");
        free_mem(archive);
        return false;
    }

    uint32_t installed = 0, failed = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t* raw = archive + MPKG_HEADER_SIZE + (uint64_t)i * MPKG_ENTRY_SIZE;
        pkg_entry_t entry;
        pkg_parse_entry(raw, &entry);

        if (pkg_extract_entry(&entry, archive, archive_size, target_dir)) {
            installed++;
        } else {
            failed++;
        }
    }

    free_mem(archive);
    if (out_installed) *out_installed = installed;
    if (out_failed) *out_failed = failed;
    return true;
}

bool pkglib_info(const char* archive_path, uint32_t* out_entry_count) {
    if (out_entry_count) *out_entry_count = 0;
    if (!archive_path) return false;

    uint32_t archive_size = 0;
    uint8_t* archive = pkg_load_archive(archive_path, &archive_size);
    if (!archive) return false;

    uint32_t entry_count = pkg_validate_entry_table(archive, archive_size);
    free_mem(archive);
    if (entry_count == 0) return false;

    if (out_entry_count) *out_entry_count = entry_count;
    return true;
}

/* ============================================================
 * ZIP
 * ============================================================ */

typedef struct {
    char rel_name[MPKG_NAME_SIZE];
    char full_path[256];
    bool is_dir;
} pkg_zip_item_t;

#define PKGLIB_ZIP_MAX_ITEMS 16
static pkg_zip_item_t g_zip_items[PKGLIB_ZIP_MAX_ITEMS];

static bool pkg_zip_add_item(pkg_zip_item_t** items, uint32_t* count, uint32_t* capacity,
                             const char* rel_name, const char* full_path, bool is_dir) {
    if (strlen(full_path) >= sizeof(((pkg_zip_item_t*)0)->full_path)) return false;

    if (*count >= *capacity) {
        return false;
    }

    pkg_zip_item_t* it = &(*items)[(*count)++];
    strncpy(it->rel_name, rel_name, sizeof(it->rel_name) - 1);
    it->rel_name[sizeof(it->rel_name) - 1] = '\0';
    strncpy(it->full_path, full_path, sizeof(it->full_path) - 1);
    it->full_path[sizeof(it->full_path) - 1] = '\0';
    it->is_dir = is_dir;
    return true;
}

static bool pkg_zip_collect_dir(const char* full_dir_path, const char* rel_prefix,
                                pkg_zip_item_t** items, uint32_t* count, uint32_t* capacity) {
    minimafs_dir_entry_t entries[MINIMAFS_MAX_ROOT_ENTRIES];

    uint32_t n = minimafs_list_dir(full_dir_path, entries, MINIMAFS_MAX_ROOT_ENTRIES);
    bool ok = true;

    for (uint32_t i = 0; i < n && ok; i++) {
        char rel_name[MPKG_NAME_SIZE];
        if (rel_prefix[0]) {
            snprintf(rel_name, sizeof(rel_name), "%s/%s", rel_prefix, entries[i].name);
        } else {
            snprintf(rel_name, sizeof(rel_name), "%s", entries[i].name);
        }

        char child_full[MINIMAFS_MAX_PATH];
        size_t base_len = strlen(full_dir_path);
        if (base_len > 0 && full_dir_path[base_len - 1] == '/') {
            snprintf(child_full, sizeof(child_full), "%s%s", full_dir_path, entries[i].name);
        } else {
            snprintf(child_full, sizeof(child_full), "%s/%s", full_dir_path, entries[i].name);
        }

        bool is_dir = (entries[i].type == MINIMAFS_TYPE_DIR);
        if (!pkg_zip_add_item(items, count, capacity, rel_name, child_full, is_dir)) {
            ok = false;
            break;
        }
        if (is_dir) {
            if (!pkg_zip_collect_dir(child_full, rel_name, items, count, capacity)) {
                ok = false;
                break;
            }
        }
    }
    return ok;
}

static void pkg_zip_free_all(pkg_zip_item_t* items,
                             uint8_t** payloads, uint32_t item_count,
                             uint32_t* payload_sizes, uint32_t* raw_sizes,
                             uint32_t* methods, uint32_t* data_offsets) {
    if (payloads) {
        for (uint32_t i = 0; i < item_count; i++) {
            if (payloads[i]) free_mem(payloads[i]);
        }
        free_mem(payloads);
    }
    if (payload_sizes) free_mem(payload_sizes);
    if (raw_sizes)     free_mem(raw_sizes);
    if (methods)       free_mem(methods);
    if (data_offsets)  free_mem(data_offsets);
    if (items && items != g_zip_items) free_mem(items);
}

bool pkglib_zip(const char* source_path, const char* algorithm,
                char* out_path, uint32_t out_path_size) {
    if (out_path && out_path_size > 0) out_path[0] = '\0';
    if (!source_path) return false;
    if (!minimafs_exists(source_path)) {
        serial_write_str("pkglib: no such file or directory: ");
        serial_write_str(source_path);
        serial_write_str("\n");
        return false;
    }

    bool use_lzss = true;
    if (algorithm && *algorithm) {
        if (strcmp(algorithm, "lzss") == 0) {
            use_lzss = true;
        } else if (strcmp(algorithm, "store") == 0) {
            use_lzss = false;
        } else {
            serial_write_str("pkglib: unknown algorithm '");
            serial_write_str(algorithm);
            serial_write_str("'\n");
            return false;
        }
    }

    pkg_zip_item_t* items = g_zip_items;
    uint32_t item_count = 0, item_capacity = PKGLIB_ZIP_MAX_ITEMS;
    bool source_is_dir = minimafs_is_dir(source_path);

    if (source_is_dir) {
        serial_write_str("pkglib: collecting archive entries\n");
        if (!pkg_zip_collect_dir(source_path, "", &items, &item_count, &item_capacity)) {
            serial_write_str("pkglib: failed to walk directory (OOM?)\n");
            pkg_zip_free_all(items, NULL, 0, NULL, NULL, NULL, NULL);
            return false;
        }
        if (item_count == 0) {
            serial_write_str("pkglib: directory is empty, nothing to archive\n");
            pkg_zip_free_all(items, NULL, 0, NULL, NULL, NULL, NULL);
            return false;
        }
    } else {
        if (!pkg_zip_add_item(&items, &item_count, &item_capacity,
                              pkg_basename(source_path), source_path, false)) {
            serial_write_str("pkglib: OOM\n");
            return false;
        }
    }

    char output_path[MINIMAFS_MAX_PATH];
    {
        char trimmed[MINIMAFS_MAX_PATH];
        strncpy(trimmed, source_path, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t len = strlen(trimmed);
        if (len > 1 && trimmed[len - 1] == '/') trimmed[len - 1] = '\0';
        if (snprintf(output_path, sizeof(output_path), "%s.mpkg", trimmed) >=
            (int)sizeof(output_path)) {
            serial_write_str("pkglib: output path too long\n");
            pkg_zip_free_all(items, NULL, 0, NULL, NULL, NULL, NULL);
            return false;
        }
    }

    uint8_t** payloads       = (uint8_t**)alloc(item_count * sizeof(uint8_t*));
    uint32_t* payload_sizes  = (uint32_t*)alloc(item_count * sizeof(uint32_t));
    uint32_t* raw_sizes      = (uint32_t*)alloc(item_count * sizeof(uint32_t));
    uint32_t* methods        = (uint32_t*)alloc(item_count * sizeof(uint32_t));

    if (!payloads || !payload_sizes || !raw_sizes || !methods) {
        serial_write_str("pkglib: OOM\n");
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
        return false;
    }

    for (uint32_t i = 0; i < item_count; i++) {
        if (items[i].is_dir) {
            payloads[i] = NULL;
            payload_sizes[i] = 0;
            raw_sizes[i] = 0;
            methods[i] = MPKG_METHOD_STORE;
            continue;
        }

        serial_write_str("pkglib: reading archive payload\n");
        minimafs_file_handle_t* f = minimafs_open(items[i].full_path, true);
        if (!f) {
            serial_write_str("pkglib: failed to open ");
            serial_write_str(items[i].full_path);
            serial_write_str("\n");
            pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
            return false;
        }

        uint32_t raw_size = minimafs_size(f);
        uint8_t* raw = raw_size ? (uint8_t*)alloc_unzeroed(raw_size) : NULL;
        if (raw_size && !raw) {
            minimafs_close(f);
            serial_write_str("pkglib: OOM reading ");
            serial_write_str(items[i].full_path);
            serial_write_str("\n");
            pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
            return false;
        }
        if (raw_size) {
            uint32_t got = minimafs_read(f, raw, raw_size);
            if (got != raw_size) {
                minimafs_close(f);
                free_mem(raw);
                serial_write_str("pkglib: short read on ");
                serial_write_str(items[i].full_path);
                serial_write_str("\n");
                pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
                return false;
            }
        }
        minimafs_close(f);

        raw_sizes[i] = raw_size;

        bool try_lzss = use_lzss && raw_size > 0 && raw_size <= PKGLIB_ZIP_LZSS_MAX_BYTES;

        if (try_lzss) {
            uint32_t compressed_size = 0;
            uint8_t* compressed = lzss_compress(raw, raw_size, &compressed_size);
            if (compressed && compressed_size < raw_size) {
                payloads[i] = compressed;
                payload_sizes[i] = compressed_size;
                methods[i] = MPKG_METHOD_LZSS;
                free_mem(raw);
            } else {
                if (compressed) free_mem(compressed);
                payloads[i] = raw;
                payload_sizes[i] = raw_size;
                methods[i] = MPKG_METHOD_STORE;
            }
        } else {
            payloads[i] = raw;
            payload_sizes[i] = raw_size;
            methods[i] = MPKG_METHOD_STORE;
        }
    }

    uint32_t table_size = MPKG_HEADER_SIZE + item_count * MPKG_ENTRY_SIZE;
    uint32_t* data_offsets = (uint32_t*)alloc(item_count * sizeof(uint32_t));
    if (!data_offsets) {
        serial_write_str("pkglib: OOM\n");
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
        return false;
    }
    uint32_t running = table_size;
    for (uint32_t i = 0; i < item_count; i++) {
        if (items[i].is_dir) { data_offsets[i] = 0; continue; }
        if (payload_sizes[i] > UINT32_MAX - running) {
            serial_write_str("pkglib: archive is too large\n");
            pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
            return false;
        }
        data_offsets[i] = running;
        running += payload_sizes[i];
    }

    uint8_t* archive = (uint8_t*)alloc(running);
    if (!archive) {
        serial_write_str("pkglib: OOM building archive\n");
        minimafs_delete_file(output_path);
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
        return false;
    }

    memset(archive, 0, running);
    memcpy(archive, MPKG_MAGIC, MPKG_MAGIC_SIZE);
    pkg_write_u32(archive + 8, MPKG_VERSION);
    pkg_write_u32(archive + 12, item_count);

    for (uint32_t i = 0; i < item_count; i++) {
        uint8_t* entry = archive + MPKG_HEADER_SIZE + i * MPKG_ENTRY_SIZE;
        strncpy((char*)entry, items[i].rel_name, MPKG_NAME_SIZE - 1);

        uint32_t off = MPKG_NAME_SIZE;
        uint32_t flags = items[i].is_dir ? MPKG_FLAG_DIRECTORY : 0;
        off += pkg_write_u32(entry + off, flags);
        off += pkg_write_u32(entry + off, methods[i]);
        off += pkg_write_u32(entry + off, raw_sizes[i]);
        off += pkg_write_u32(entry + off, payload_sizes[i]);
        off += pkg_write_u32(entry + off, data_offsets[i]);
    }

    for (uint32_t i = 0; i < item_count; i++) {
        if (!items[i].is_dir && payload_sizes[i] > 0) {
            memcpy(archive + data_offsets[i], payloads[i], payload_sizes[i]);
        }
    }

    serial_write_str("pkglib: writing archive\n");
    bool write_ok = minimafs_write_file_segments(output_path, archive, running,
                                                 NULL, 0, "binary", "mpkg");
    free_mem(archive);

    if (!write_ok) {
        serial_write_str("pkglib: write failed: ");
        serial_write_str(output_path);
        serial_write_str("\n");
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
        return false;
    }

    if (out_path && out_path_size > 0) {
        strncpy(out_path, output_path, out_path_size - 1);
        out_path[out_path_size - 1] = '\0';
    }

    pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
    return true;
}