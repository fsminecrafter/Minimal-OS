/*
 * pkg / installpkg / pkginfo — build, extract, and inspect a
 * MinimalOS Package (.mpkg) archive, the MinimalOS analog of a zip
 * file.
 *
 * See x86_64/pkgformat.h for the on-disk layout and x86_64/lzss.h for
 * the (de)compressor.
 *
 * Usage:
 *   pkg unzip <path.mpkg> [target-dir]
 *   pkg zip   <file-or-folder> [algorithm]
 *   installpkg <path.mpkg> [target-dir]   (legacy alias for `pkg unzip`)
 *   pkginfo    <path.mpkg>
 *
 * `pkg zip` writes its output next to the source as
 * "<source-path>.mpkg" (trailing '/' stripped for directories).
 * `algorithm` is "lzss" (default) or "store"; any other value is
 * rejected. Archives can also be built on the host with
 * tools/mkpkg/mkpkg.py.
 */

#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/allocator.h"
#include "x86_64/minimafs.h"
#include "x86_64/pkgformat.h"
#include "x86_64/lzss.h"
#include "fspaths.h"

/* Above this raw file size, `pkg zip` silently falls back to STORE
 * even if lzss was requested. The brute-force encoder is
 * O(input_size * N * F) - fine for typical program bundles, but an
 * unbounded multi-megabyte input could tie up the calling command
 * process for a very long time. */
#define PKG_ZIP_LZSS_MAX_BYTES (256u * 1024u)

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
    out->name[MPKG_NAME_SIZE - 1] = '\0'; // defensive against a malformed archive

    const uint8_t* p = raw + MPKG_NAME_SIZE;
    out->flags            = pkg_read_u32(p + 0);
    out->method            = pkg_read_u32(p + 4);
    out->uncompressed_size = pkg_read_u32(p + 8);
    out->compressed_size   = pkg_read_u32(p + 12);
    out->data_offset       = pkg_read_u32(p + 16);
}

// Rejects an archive entry name that tries to escape the install
// target (absolute paths, ".." components). Archives are just data
// files someone imported - never trust their contents to stay inside
// the intended directory on their own.
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

// Creates every directory component of `path` (an "N:/a/b/c" style
// fully-resolved MinimaFS path), including the final component
// itself. Safe to call on a path that already exists as a directory.
static bool pkg_mkdir_p(const char* path) {
    if (!path) return false;

    size_t len = strlen(path);
    if (len == 0 || len >= MINIMAFS_MAX_PATH) return false;

    char buf[MINIMAFS_MAX_PATH];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char* colon = strchr(buf, ':');
    if (!colon || colon[1] != '/') return false;
    size_t min_len = (size_t)(colon - buf) + 2; // past "N:/"
    if (min_len > len) return false;

    for (size_t i = min_len; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            bool at_end = (buf[i] == '\0');
            char saved = buf[i];
            buf[i] = '\0';
            if (i > min_len) {
                if (!minimafs_mkdir(buf)) {
                    return false;
                }
            }
            buf[i] = saved;
            if (at_end) break;
        }
    }
    return true;
}

// Splits a full "N:/a/b/c" path into its parent directory. Handles the
// drive-root case correctly ("0:/hello.run" -> "0:/", not "0:").
static bool pkg_parent_dir(const char* full_path, char* out, size_t out_size) {
    const char* colon = strchr(full_path, ':');
    if (!colon || colon[1] != '/') return false;
    const char* root_slash = colon + 1;
    const char* last_slash = strrchr(full_path, '/');
    if (!last_slash) return false;

    size_t len = (last_slash == root_slash)
        ? (size_t)(last_slash - full_path) + 1   // keep root's own '/'
        : (size_t)(last_slash - full_path);

    if (len >= out_size) return false;
    memcpy(out, full_path, len);
    out[len] = '\0';
    return true;
}

// Returns the last path component of `path` ("a/b/c.run" -> "c.run").
static const char* pkg_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool pkg_extract_entry(const pkg_entry_t* entry, const uint8_t* archive,
                              uint32_t archive_size, const char* target_dir) {
    if (!pkg_name_is_safe(entry->name)) {
        graphics_write_textr("installpkg: unsafe entry name, skipping: ");
        graphics_write_textr(entry->name[0] ? entry->name : "(empty)");
        graphics_write_textr("\n");
        return false;
    }

    char full_path[MINIMAFS_MAX_PATH];
    int written = snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, entry->name);
    if (written <= 0 || (size_t)written >= sizeof(full_path)) {
        graphics_write_textr("installpkg: path too long: ");
        graphics_write_textr(entry->name);
        graphics_write_textr("\n");
        return false;
    }

    if (entry->flags & MPKG_FLAG_DIRECTORY) {
        if (!pkg_mkdir_p(full_path)) {
            graphics_write_textr("installpkg: failed to create directory ");
            graphics_write_textr(full_path);
            graphics_write_textr("\n");
            return false;
        }
        serial_write_str("installpkg: created dir ");
        serial_write_str(full_path);
        serial_write_str("\n");
        return true;
    }

    char parent[MINIMAFS_MAX_PATH];
    if (pkg_parent_dir(full_path, parent, sizeof(parent))) {
        if (!pkg_mkdir_p(parent)) {
            graphics_write_textr("installpkg: failed to create parent dir for ");
            graphics_write_textr(entry->name);
            graphics_write_textr("\n");
            return false;
        }
    }

    // Bounds-check the payload range before touching it.
    if (entry->compressed_size > 0) {
        uint64_t end = (uint64_t)entry->data_offset + (uint64_t)entry->compressed_size;
        if (entry->data_offset >= archive_size || end > archive_size) {
            graphics_write_textr("installpkg: corrupt archive (bad data range) for ");
            graphics_write_textr(entry->name);
            graphics_write_textr("\n");
            return false;
        }
    }

    const uint8_t* payload = archive + entry->data_offset;
    bool ok;

    if (entry->method == MPKG_METHOD_STORE) {
        if (entry->uncompressed_size != entry->compressed_size) {
            graphics_write_textr("installpkg: corrupt archive (size mismatch) for ");
            graphics_write_textr(entry->name);
            graphics_write_textr("\n");
            return false;
        }
        ok = minimafs_write_file_segments(full_path, payload, entry->compressed_size,
                                          NULL, 0, "binary", "bin");
    } else if (entry->method == MPKG_METHOD_LZSS) {
        uint8_t* decompressed = entry->uncompressed_size
            ? (uint8_t*)alloc_unzeroed(entry->uncompressed_size)
            : NULL;
        if (entry->uncompressed_size && !decompressed) {
            graphics_write_textr("installpkg: OOM decompressing ");
            graphics_write_textr(entry->name);
            graphics_write_textr("\n");
            return false;
        }

        uint32_t produced = entry->uncompressed_size
            ? lzss_decompress(payload, entry->compressed_size,
                              decompressed, entry->uncompressed_size)
            : 0;

        if (entry->uncompressed_size && produced != entry->uncompressed_size) {
            graphics_write_textr("installpkg: decompression failed for ");
            graphics_write_textr(entry->name);
            graphics_write_textr("\n");
            if (decompressed) free_mem(decompressed);
            return false;
        }

        ok = minimafs_write_file_segments(full_path, decompressed, entry->uncompressed_size,
                                          NULL, 0, "binary", "bin");
        if (decompressed) free_mem(decompressed);
    } else {
        graphics_write_textr("installpkg: unknown compression method for ");
        graphics_write_textr(entry->name);
        graphics_write_textr("\n");
        return false;
    }

    if (!ok) {
        graphics_write_textr("installpkg: failed to write ");
        graphics_write_textr(full_path);
        graphics_write_textr("\n");
        return false;
    }

    serial_write_str("installpkg: wrote ");
    serial_write_str(full_path);
    serial_write_str("\n");
    return true;
}

// Reads the whole archive into memory and validates its header. On
// success returns the heap buffer (caller frees) and sets *out_size.
// Returns NULL on any failure (and prints a message).
static uint8_t* pkg_load_archive(const char* path, uint32_t* out_size) {
    minimafs_file_handle_t* file = minimafs_open(path, true);
    if (!file) {
        graphics_write_textr("installpkg: cannot open ");
        graphics_write_textr(path);
        graphics_write_textr("\n");
        return NULL;
    }

    uint32_t size = minimafs_size(file);
    if (size < MPKG_HEADER_SIZE) {
        graphics_write_textr("installpkg: file too small to be a package\n");
        minimafs_close(file);
        return NULL;
    }

    uint8_t* buffer = (uint8_t*)alloc_unzeroed(size);
    if (!buffer) {
        graphics_write_textr("installpkg: out of memory reading archive\n");
        minimafs_close(file);
        return NULL;
    }

    uint32_t got = minimafs_read(file, buffer, size);
    minimafs_close(file);

    if (got != size) {
        graphics_write_textr("installpkg: short read on archive\n");
        free_mem(buffer);
        return NULL;
    }

    if (memcmp(buffer, MPKG_MAGIC, MPKG_MAGIC_SIZE) != 0) {
        graphics_write_textr("installpkg: not a valid .mpkg file (bad magic)\n");
        free_mem(buffer);
        return NULL;
    }

    uint32_t version = pkg_read_u32(buffer + 8);
    if (version != MPKG_VERSION) {
        graphics_write_textr("installpkg: unsupported package version\n");
        free_mem(buffer);
        return NULL;
    }

    *out_size = size;
    return buffer;
}

// Validates and returns the entry count for an already-loaded archive,
// or 0 (with a message) if the entry table doesn't fit in the file.
static uint32_t pkg_validate_entry_table(const uint8_t* archive, uint32_t archive_size) {
    uint32_t entry_count = pkg_read_u32(archive + 12);
    uint64_t table_bytes = (uint64_t)entry_count * MPKG_ENTRY_SIZE;

    if (entry_count == 0 || table_bytes > (uint64_t)archive_size - MPKG_HEADER_SIZE) {
        graphics_write_textr("installpkg: corrupt archive (bad entry table)\n");
        return 0;
    }
    return entry_count;
}

/* ============================================================
 * SHARED EXTRACTION LOGIC (used by both `installpkg` and `pkg unzip`)
 * ============================================================ */

static void pkg_do_unzip(const char* archive_arg, const char* target_arg) {
    if (!archive_arg) {
        graphics_write_textr("Usage: pkg unzip <path.mpkg> [target-dir]\n");
        return;
    }

    char archive_path[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(archive_arg, archive_path)) {
        graphics_write_textr("installpkg: invalid archive path\n");
        return;
    }

    char target_dir[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(target_arg ? target_arg : "", target_dir)) {
        graphics_write_textr("installpkg: invalid target directory\n");
        return;
    }

    uint32_t archive_size = 0;
    uint8_t* archive = pkg_load_archive(archive_path, &archive_size);
    if (!archive) return;

    uint32_t entry_count = pkg_validate_entry_table(archive, archive_size);
    if (entry_count == 0) {
        free_mem(archive);
        return;
    }

    if (!minimafs_is_dir(target_dir) && !pkg_mkdir_p(target_dir)) {
        graphics_write_textr("installpkg: could not create target directory\n");
        free_mem(archive);
        return;
    }

    graphics_write_textr("Installing ");
    graphics_write_textr_udec(entry_count);
    graphics_write_textr(" entries from ");
    graphics_write_textr(archive_arg);
    graphics_write_textr(" into ");
    graphics_write_textr(target_dir);
    graphics_write_textr("\n");

    uint32_t installed = 0;
    uint32_t failed = 0;

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

    graphics_write_textr("installpkg: ");
    graphics_write_textr_udec(installed);
    graphics_write_textr(" installed, ");
    graphics_write_textr_udec(failed);
    graphics_write_textr(" failed\n");
}

void cmd_installpkg(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: installpkg <path.mpkg> [target-dir]\n");
        return;
    }
    pkg_do_unzip(argv[1], argc >= 3 ? argv[2] : NULL);
}

void cmd_pkginfo(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: pkginfo <path.mpkg>\n");
        return;
    }

    char archive_path[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(argv[1], archive_path)) {
        graphics_write_textr("pkginfo: invalid path\n");
        return;
    }

    uint32_t archive_size = 0;
    uint8_t* archive = pkg_load_archive(archive_path, &archive_size);
    if (!archive) return;

    uint32_t entry_count = pkg_validate_entry_table(archive, archive_size);
    if (entry_count == 0) {
        free_mem(archive);
        return;
    }

    graphics_write_textr("Package: ");
    graphics_write_textr(argv[1]);
    graphics_write_textr("\n");
    graphics_write_textr_udec(entry_count);
    graphics_write_textr(" entries:\n");

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t* raw = archive + MPKG_HEADER_SIZE + (uint64_t)i * MPKG_ENTRY_SIZE;
        pkg_entry_t entry;
        pkg_parse_entry(raw, &entry);

        graphics_write_textr("  ");
        graphics_write_textr(entry.name);
        if (entry.flags & MPKG_FLAG_DIRECTORY) {
            graphics_write_textr(" [DIR]\n");
            continue;
        }
        graphics_write_textr(" (");
        graphics_write_textr_udec(entry.uncompressed_size);
        graphics_write_textr(" bytes");
        if (entry.method == MPKG_METHOD_LZSS) {
            graphics_write_textr(", compressed to ");
            graphics_write_textr_udec(entry.compressed_size);
        }
        graphics_write_textr(")\n");
    }

    free_mem(archive);
}

/* ============================================================
 * ZIP (ARCHIVE BUILDING)
 * ============================================================ */

typedef struct {
    char rel_name[MPKG_NAME_SIZE];      // name inside the archive
    char full_path[MINIMAFS_MAX_PATH];  // where to read it from on MinimaFS
    bool is_dir;
} pkg_zip_item_t;

static bool pkg_zip_add_item(pkg_zip_item_t** items, uint32_t* count, uint32_t* capacity,
                             const char* rel_name, const char* full_path, bool is_dir) {
    if (*count >= *capacity) {
        uint32_t new_cap = (*capacity == 0) ? 16 : (*capacity * 2);
        pkg_zip_item_t* bigger =
            (pkg_zip_item_t*)alloc_resize(*items, (size_t)new_cap * sizeof(pkg_zip_item_t));
        if (!bigger) return false;
        *items = bigger;
        *capacity = new_cap;
    }

    pkg_zip_item_t* it = &(*items)[(*count)++];
    strncpy(it->rel_name, rel_name, sizeof(it->rel_name) - 1);
    it->rel_name[sizeof(it->rel_name) - 1] = '\0';
    strncpy(it->full_path, full_path, sizeof(it->full_path) - 1);
    it->full_path[sizeof(it->full_path) - 1] = '\0';
    it->is_dir = is_dir;
    return true;
}

// Recursively walks `full_dir_path`, adding every file and directory
// under it to `items` with archive-relative names built from
// `rel_prefix` ("" at the top level).
static bool pkg_zip_collect_dir(const char* full_dir_path, const char* rel_prefix,
                                pkg_zip_item_t** items, uint32_t* count, uint32_t* capacity) {
    minimafs_dir_entry_t* entries = (minimafs_dir_entry_t*)alloc(
        MINIMAFS_MAX_ROOT_ENTRIES * sizeof(minimafs_dir_entry_t));
    if (!entries) return false;

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

    free_mem(entries);
    return ok;
}

// Frees everything pkg_do_zip() may have allocated. Safe to call with
// any subset of pointers NULL / item_count 0.
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
    if (items)         free_mem(items);
}

static void pkg_do_zip(const char* source_arg, const char* algo_arg) {
    if (!source_arg) {
        graphics_write_textr("Usage: pkg zip <file-or-folder> [algorithm]\n");
        return;
    }

    char resolved[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(source_arg, resolved)) {
        graphics_write_textr("pkg zip: invalid path\n");
        return;
    }
    if (!minimafs_exists(resolved)) {
        graphics_write_textr("pkg zip: no such file or directory: ");
        graphics_write_textr(source_arg);
        graphics_write_textr("\n");
        return;
    }

    bool use_lzss = true; // default, per requested syntax ("leave empty for default")
    if (algo_arg && *algo_arg) {
        if (strcmp(algo_arg, "lzss") == 0) {
            use_lzss = true;
        } else if (strcmp(algo_arg, "store") == 0) {
            use_lzss = false;
        } else {
            graphics_write_textr("pkg zip: unknown algorithm '");
            graphics_write_textr(algo_arg);
            graphics_write_textr("' (supported: lzss, store)\n");
            return;
        }
    }

    pkg_zip_item_t* items = NULL;
    uint32_t item_count = 0, item_capacity = 0;
    bool source_is_dir = minimafs_is_dir(resolved);

    if (source_is_dir) {
        if (!pkg_zip_collect_dir(resolved, "", &items, &item_count, &item_capacity)) {
            graphics_write_textr("pkg zip: failed to walk directory (OOM?)\n");
            pkg_zip_free_all(items, NULL, 0, NULL, NULL, NULL, NULL);
            return;
        }
        if (item_count == 0) {
            graphics_write_textr("pkg zip: directory is empty, nothing to archive\n");
            pkg_zip_free_all(items, NULL, 0, NULL, NULL, NULL, NULL);
            return;
        }
    } else {
        if (!pkg_zip_add_item(&items, &item_count, &item_capacity,
                              pkg_basename(resolved), resolved, false)) {
            graphics_write_textr("pkg zip: OOM\n");
            return;
        }
    }

    // Output path: "<source-without-trailing-slash>.mpkg", next to the source.
    char output_path[MINIMAFS_MAX_PATH];
    {
        char trimmed[MINIMAFS_MAX_PATH];
        strncpy(trimmed, resolved, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t len = strlen(trimmed);
        if (len > 1 && trimmed[len - 1] == '/') trimmed[len - 1] = '\0';
        if (snprintf(output_path, sizeof(output_path), "%s.mpkg", trimmed) >=
            (int)sizeof(output_path)) {
            graphics_write_textr("pkg zip: output path too long\n");
            pkg_zip_free_all(items, NULL, 0, NULL, NULL, NULL, NULL);
            return;
        }
    }

    uint8_t** payloads       = (uint8_t**)alloc(item_count * sizeof(uint8_t*));
    uint32_t* payload_sizes  = (uint32_t*)alloc(item_count * sizeof(uint32_t));
    uint32_t* raw_sizes      = (uint32_t*)alloc(item_count * sizeof(uint32_t));
    uint32_t* methods        = (uint32_t*)alloc(item_count * sizeof(uint32_t));

    if (!payloads || !payload_sizes || !raw_sizes || !methods) {
        graphics_write_textr("pkg zip: OOM\n");
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
        return;
    }

    bool capped_notice_shown = false;

    for (uint32_t i = 0; i < item_count; i++) {
        if (items[i].is_dir) {
            payloads[i] = NULL;
            payload_sizes[i] = 0;
            raw_sizes[i] = 0;
            methods[i] = MPKG_METHOD_STORE;
            continue;
        }

        minimafs_file_handle_t* f = minimafs_open(items[i].full_path, true);
        if (!f) {
            graphics_write_textr("pkg zip: failed to open ");
            graphics_write_textr(items[i].full_path);
            graphics_write_textr("\n");
            pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
            return;
        }

        uint32_t raw_size = minimafs_size(f);
        uint8_t* raw = raw_size ? (uint8_t*)alloc_unzeroed(raw_size) : NULL;
        if (raw_size && !raw) {
            minimafs_close(f);
            graphics_write_textr("pkg zip: OOM reading ");
            graphics_write_textr(items[i].full_path);
            graphics_write_textr("\n");
            pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
            return;
        }
        if (raw_size) {
            uint32_t got = minimafs_read(f, raw, raw_size);
            if (got != raw_size) {
                minimafs_close(f);
                free_mem(raw);
                graphics_write_textr("pkg zip: short read on ");
                graphics_write_textr(items[i].full_path);
                graphics_write_textr("\n");
                pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
                return;
            }
        }
        minimafs_close(f);

        raw_sizes[i] = raw_size;

        bool try_lzss = use_lzss && raw_size > 0;
        if (try_lzss && raw_size > PKG_ZIP_LZSS_MAX_BYTES) {
            try_lzss = false;
            if (!capped_notice_shown) {
                graphics_write_textr("pkg zip: note: large file(s) stored uncompressed "
                                     "(over size cap for lzss)\n");
                capped_notice_shown = true;
            }
        }

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

    // Compute per-entry payload offsets.
    uint32_t table_size = MPKG_HEADER_SIZE + item_count * MPKG_ENTRY_SIZE;
    uint32_t* data_offsets = (uint32_t*)alloc(item_count * sizeof(uint32_t));
    if (!data_offsets) {
        graphics_write_textr("pkg zip: OOM\n");
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, NULL);
        return;
    }
    uint32_t running = table_size;
    for (uint32_t i = 0; i < item_count; i++) {
        if (items[i].is_dir) { data_offsets[i] = 0; continue; }
        data_offsets[i] = running;
        running += payload_sizes[i];
    }

    if (minimafs_exists(output_path)) {
        minimafs_delete_file(output_path);
    }
    if (!minimafs_create_file(output_path, "binary", "mpkg")) {
        graphics_write_textr("pkg zip: failed to create ");
        graphics_write_textr(output_path);
        graphics_write_textr("\n");
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
        return;
    }

    minimafs_file_handle_t* out = minimafs_open(output_path, false);
    if (!out) {
        graphics_write_textr("pkg zip: failed to open output for writing\n");
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
        return;
    }

    bool write_ok = true;

    // Header
    {
        uint8_t header[MPKG_HEADER_SIZE];
        memcpy(header, MPKG_MAGIC, MPKG_MAGIC_SIZE);
        pkg_write_u32(header + 8, MPKG_VERSION);
        pkg_write_u32(header + 12, item_count);
        if (minimafs_write(out, header, sizeof(header)) != sizeof(header)) write_ok = false;
    }

    // Entry table
    for (uint32_t i = 0; write_ok && i < item_count; i++) {
        uint8_t entry[MPKG_ENTRY_SIZE];
        memset(entry, 0, sizeof(entry));
        strncpy((char*)entry, items[i].rel_name, MPKG_NAME_SIZE - 1);

        uint32_t off = MPKG_NAME_SIZE;
        uint32_t flags = items[i].is_dir ? MPKG_FLAG_DIRECTORY : 0;
        off += pkg_write_u32(entry + off, flags);
        off += pkg_write_u32(entry + off, methods[i]);
        off += pkg_write_u32(entry + off, raw_sizes[i]);
        off += pkg_write_u32(entry + off, payload_sizes[i]);
        off += pkg_write_u32(entry + off, data_offsets[i]);

        if (minimafs_write(out, entry, sizeof(entry)) != sizeof(entry)) write_ok = false;
    }

    // Payloads
    for (uint32_t i = 0; write_ok && i < item_count; i++) {
        if (!items[i].is_dir && payload_sizes[i] > 0) {
            if (minimafs_write(out, payloads[i], payload_sizes[i]) != payload_sizes[i]) {
                write_ok = false;
            }
        }
    }

    minimafs_close(out);

    if (!write_ok) {
        graphics_write_textr("pkg zip: write failed, archive may be incomplete: ");
        graphics_write_textr(output_path);
        graphics_write_textr("\n");
        minimafs_delete_file(output_path);
        pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
        return;
    }

    graphics_write_textr("pkg zip: wrote ");
    graphics_write_textr(output_path);
    graphics_write_textr(" (");
    graphics_write_textr_udec(item_count);
    graphics_write_textr(" entries)\n");

    pkg_zip_free_all(items, payloads, item_count, payload_sizes, raw_sizes, methods, data_offsets);
}

/* ============================================================
 * `pkg` DISPATCHER
 * ============================================================ */

void cmd_pkg(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage:\n");
        graphics_write_textr("  pkg unzip <path.mpkg> [target-dir]\n");
        graphics_write_textr("  pkg zip <file-or-folder> [algorithm]\n");
        graphics_write_textr("    algorithm: lzss (default) or store\n");
        return;
    }

    if (strcmp(argv[1], "unzip") == 0) {
        if (argc < 3) {
            graphics_write_textr("Usage: pkg unzip <path.mpkg> [target-dir]\n");
            return;
        }
        pkg_do_unzip(argv[2], argc >= 4 ? argv[3] : NULL);
    } else if (strcmp(argv[1], "zip") == 0) {
        if (argc < 3) {
            graphics_write_textr("Usage: pkg zip <file-or-folder> [algorithm]\n");
            return;
        }
        pkg_do_zip(argv[2], argc >= 4 ? argv[3] : NULL);
    } else {
        graphics_write_textr("pkg: unknown subcommand '");
        graphics_write_textr(argv[1]);
        graphics_write_textr("'. Use zip|unzip\n");
    }
}

void register_pkg_commands(void) {
    command_register("installpkg", cmd_installpkg);
    command_register("pkginfo", cmd_pkginfo);
    command_register("pkg", cmd_pkg);
}

REGISTER_COMMAND(register_pkg_commands);