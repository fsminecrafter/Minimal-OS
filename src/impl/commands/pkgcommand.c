/*
 * installpkg / pkginfo — extract or inspect a MinimalOS Package
 * (.mpkg) archive, the MinimalOS analog of unzipping an installer.
 *
 * See x86_64/pkgformat.h for the on-disk layout and x86_64/lzss.h for
 * the decompressor. Archives are built on the host with
 * tools/pkgbuilder/mkpkg.py.
 *
 * Usage:
 *   installpkg <path-to.mpkg> [target-dir]
 *   pkginfo    <path-to.mpkg>
 *
 * target-dir defaults to the current directory (see fspaths.h). Every
 * file in the archive is written at <target-dir>/<entry-name>, creating
 * any missing parent directories along the way.
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

static uint32_t pkg_read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

void cmd_installpkg(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: installpkg <path.mpkg> [target-dir]\n");
        return;
    }

    char archive_path[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(argv[1], archive_path)) {
        graphics_write_textr("installpkg: invalid archive path\n");
        return;
    }

    char target_dir[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(argc >= 3 ? argv[2] : "", target_dir)) {
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
    graphics_write_textr(argv[1]);
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

void register_pkg_commands(void) {
    command_register("installpkg", cmd_installpkg);
    command_register("pkginfo", cmd_pkginfo);
}

REGISTER_COMMAND(register_pkg_commands);
