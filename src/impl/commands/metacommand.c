/*
 * meta command — inspect and edit a MinimaFS file's own metadata (the
 * tagged @HEADER@ block described in minimafs.txt / exposed as
 * minimafs_file_metadata_t).
 *
 * Usage:
 *   meta <path>                - show all fields
 *   meta <path> <key> <value>  - set one field
 *
 * Keys:
 *   executable  true|false   - RUNNABLE flag. Setting this true is
 *                               what lets the command dispatcher's
 *                               file-association fallback
 *                               (commandhandler.c) invoke this file
 *                               directly by path (e.g. "./exec.run" or
 *                               even an extension-less name), not just
 *                               files that already have a mapped
 *                               extension.
 *   hidden      true|false   - HIDDEN flag
 *   filetype    <string>     - e.g. "text", "binary", "executable"
 *   fileformat  <string>     - e.g. "txt", "bin", "elf"
 *   runwith     <path>       - interpreter path (only meaningful if
 *                               there's no entrypoint set)
 *   entrypoint  <hex|dec>    - entry point address, e.g. 0x401000
 *
 * Setting one key round-trips the WHOLE metadata struct through
 * minimafs_get_metadata()/minimafs_set_metadata() (already used
 * elsewhere - see kbrcommands.c's persistence path) - only the field
 * this command touches actually changes on disk.
 */

#include <stdbool.h>
#include <stdint.h>
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/minimafs.h"
#include "fspaths.h"

static bool meta_parse_bool(const char* value, bool* out) {
    if (!value || !out) return false;
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
        strcmp(value, "yes")  == 0 || strcmp(value, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
        strcmp(value, "no")    == 0 || strcmp(value, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool meta_parse_u64(const char* value, uint64_t* out) {
    if (!value || !*value || !out) return false;
    uint64_t result = 0;

    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        const char* p = value + 2;
        if (!*p) return false;
        for (; *p; p++) {
            uint8_t digit;
            if (*p >= '0' && *p <= '9')      digit = (uint8_t)(*p - '0');
            else if (*p >= 'a' && *p <= 'f') digit = (uint8_t)(*p - 'a' + 10);
            else if (*p >= 'A' && *p <= 'F') digit = (uint8_t)(*p - 'A' + 10);
            else return false;
            result = (result << 4) | digit;
        }
    } else {
        for (const char* p = value; *p; p++) {
            if (*p < '0' || *p > '9') return false;
            result = result * 10 + (uint64_t)(*p - '0');
        }
    }

    *out = result;
    return true;
}

static void meta_print_bool(const char* label, bool value) {
    graphics_write_textr(label);
    graphics_write_textr(value ? "true\n" : "false\n");
}

static void meta_show(const char* display_path, minimafs_file_metadata_t* metadata) {
    graphics_write_textr("Metadata for ");
    graphics_write_textr(display_path);
    graphics_write_textr(":\n");

    graphics_write_textr("  filetype:    ");
    graphics_write_textr(metadata->filetype);
    graphics_write_textr("\n");

    graphics_write_textr("  fileformat:  ");
    graphics_write_textr(metadata->fileformat);
    graphics_write_textr("\n");

    meta_print_bool("  executable:  ", metadata->runnable);
    meta_print_bool("  hidden:      ", metadata->hidden);

    if (metadata->runnable) {
        if (metadata->entrypoint != 0) {
            graphics_write_textr("  entrypoint:  0x");
            graphics_write_textr_hex(metadata->entrypoint);
            graphics_write_textr("\n");
        }
        if (metadata->run_with[0] != '\0') {
            graphics_write_textr("  runwith:     ");
            graphics_write_textr(metadata->run_with);
            graphics_write_textr("\n");
        }
    }

    graphics_write_textr("  data length: ");
    graphics_write_textr_udec(metadata->data_length);
    graphics_write_textr(" bytes\n");
}

void cmd_meta(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: meta <path> [key value]\n");
        graphics_write_textr("Keys: executable, hidden, filetype, fileformat, runwith, entrypoint\n");
        return;
    }

    char resolved[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(argv[1], resolved)) {
        graphics_write_textr("meta: invalid path\n");
        return;
    }

    if (!minimafs_exists(resolved)) {
        graphics_write_textr("meta: no such file: ");
        graphics_write_textr(argv[1]);
        graphics_write_textr("\n");
        return;
    }
    if (minimafs_is_dir(resolved)) {
        graphics_write_textr("meta: cannot apply metadata to a directory\n");
        return;
    }

    minimafs_file_metadata_t metadata;
    if (!minimafs_get_metadata(resolved, &metadata)) {
        graphics_write_textr("meta: failed to read metadata\n");
        return;
    }

    if (argc < 4) {
        // No key/value given - just display what's there.
        meta_show(argv[1], &metadata);
        return;
    }

    const char* key   = argv[2];
    const char* value = argv[3];
    bool changed = false;

    if (strcmp(key, "executable") == 0 || strcmp(key, "runnable") == 0) {
        bool flag;
        if (!meta_parse_bool(value, &flag)) {
            graphics_write_textr("meta: 'executable' expects true|false\n");
            return;
        }
        metadata.runnable = flag;
        changed = true;
    } else if (strcmp(key, "hidden") == 0) {
        bool flag;
        if (!meta_parse_bool(value, &flag)) {
            graphics_write_textr("meta: 'hidden' expects true|false\n");
            return;
        }
        metadata.hidden = flag;
        changed = true;
    } else if (strcmp(key, "filetype") == 0) {
        strncpy(metadata.filetype, value, sizeof(metadata.filetype) - 1);
        metadata.filetype[sizeof(metadata.filetype) - 1] = '\0';
        changed = true;
    } else if (strcmp(key, "fileformat") == 0) {
        strncpy(metadata.fileformat, value, sizeof(metadata.fileformat) - 1);
        metadata.fileformat[sizeof(metadata.fileformat) - 1] = '\0';
        changed = true;
    } else if (strcmp(key, "runwith") == 0) {
        strncpy(metadata.run_with, value, sizeof(metadata.run_with) - 1);
        metadata.run_with[sizeof(metadata.run_with) - 1] = '\0';
        changed = true;
    } else if (strcmp(key, "entrypoint") == 0) {
        uint64_t entry;
        if (!meta_parse_u64(value, &entry)) {
            graphics_write_textr("meta: 'entrypoint' expects a hex (0x...) or decimal value\n");
            return;
        }
        metadata.entrypoint = entry;
        changed = true;
    } else {
        graphics_write_textr("meta: unknown key '");
        graphics_write_textr(key);
        graphics_write_textr("'\n");
        graphics_write_textr("Keys: executable, hidden, filetype, fileformat, runwith, entrypoint\n");
        return;
    }

    if (!changed) return;

    if (!minimafs_set_metadata(resolved, &metadata)) {
        graphics_write_textr("meta: failed to write metadata\n");
        return;
    }

    graphics_write_textr("Updated ");
    graphics_write_textr(key);
    graphics_write_textr(" on ");
    graphics_write_textr(argv[1]);
    graphics_write_textr("\n");
}

void register_meta(void) {
    command_register("meta", cmd_meta);
}

REGISTER_COMMAND(register_meta);