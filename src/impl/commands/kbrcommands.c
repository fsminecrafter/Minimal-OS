/*
 * kbrlayout command — inspect and change the active USB keyboard layout.
 *
 * Subcommands:
 *   kbrlayout get                 - show the currently loaded layout
 *   kbrlayout set <name|path>     - load a built-in layout (e.g. "US",
 *                                   "SE") or a .kbr file path
 *                                   (e.g. "0:/kbr/german.kbr")
 *   kbrlayout list                - list built-in layouts and any
 *                                   .kbr files found under 0:/kbr
 *
 * `set` also persists the chosen layout into 0:/Etc/System.conf under
 * the KeyboardLayout= key, so it can be inspected later or (manually,
 * by calling `kbrlayout set` again after boot) restored.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "print.h"
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/allocator.h"
#include "x86_64/minimafs.h"
#include "keyboard/usbkeyboard.h"

#define KBRLAYOUT_CONFIG_PATH "0:/Etc/System.conf"
#define KBRLAYOUT_CONFIG_KEY  "KeyboardLayout"
#define KBRLAYOUT_KBR_DIR     "0:/kbr"

/*
 * Heap-owned buffer backing the currently loaded .kbr file (if the
 * active layout came from disk rather than a built-in). Kept alive for
 * as long as it's the active layout, since usb_keyboard_load_kbr()
 * only stores a pointer into it - freeing it out from under the
 * keyboard driver would corrupt every subsequent keystroke.
 *
 * Only freed here, right after a *replacement* layout has already
 * been successfully loaded, so we never free memory the keyboard
 * driver is still actively using.
 */
static keyboard_layout_t* g_loaded_kbr_file = NULL;

/* ============================================================
 * Load a .kbr file from MinimaFS and activate it
 * ============================================================ */

static bool kbrlayout_load_from_file(const char* path) {
    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) {
        serial_write_str("kbrlayout: could not open file: ");
        serial_write_str(path);
        serial_write_str("\n");
        return false;
    }

    uint32_t size = minimafs_size(f);
    if (size < sizeof(keyboard_layout_t)) {
        serial_write_str("kbrlayout: file too small to be a valid .kbr\n");
        minimafs_close(f);
        return false;
    }

    uint8_t* buffer = (uint8_t*)alloc(sizeof(keyboard_layout_t));
    if (!buffer) {
        serial_write_str("kbrlayout: OOM allocating layout buffer\n");
        minimafs_close(f);
        return false;
    }

    uint32_t got = minimafs_read(f, buffer, sizeof(keyboard_layout_t));
    minimafs_close(f);

    if (got != sizeof(keyboard_layout_t)) {
        serial_write_str("kbrlayout: short read on .kbr file\n");
        free_mem(buffer);
        return false;
    }

    if (!usb_keyboard_validate_kbr(buffer, got)) {
        serial_write_str("kbrlayout: invalid .kbr file contents\n");
        free_mem(buffer);
        return false;
    }

    if (!usb_keyboard_load_kbr(buffer, got)) {
        serial_write_str("kbrlayout: usb_keyboard_load_kbr failed\n");
        free_mem(buffer);
        return false;
    }

    /* Only now, after the new layout is fully active, is it safe to
     * free the previous file-backed layout buffer (if any). */
    if (g_loaded_kbr_file) {
        free_mem(g_loaded_kbr_file);
    }
    g_loaded_kbr_file = (keyboard_layout_t*)buffer;

    return true;
}

/* ============================================================
 * Persist the chosen layout selector into 0:/Etc/System.conf
 *
 * Rewrites the KeyboardLayout= line (or appends it if missing) while
 * preserving every other line in the file. Directly rebuilds the
 * file handle's in-memory buffer rather than relying on
 * minimafs_write()'s append-only semantics, since the new content can
 * legitimately be shorter than the old content.
 * ============================================================ */

static bool kbrlayout_persist_setting(const char* value) {
    if (!value) return false;

    minimafs_file_handle_t* f = minimafs_open(KBRLAYOUT_CONFIG_PATH, false);
    if (!f) {
        serial_write_str("kbrlayout: could not open ");
        serial_write_str(KBRLAYOUT_CONFIG_PATH);
        serial_write_str(" to persist setting (is the disk mounted?)\n");
        return false;
    }

    uint32_t old_size = f->data_size;
    const char* old_data = (const char*)f->data; /* may be NULL if empty */

    uint32_t new_capacity = old_size + 256;
    char* new_data = (char*)alloc_unzeroed(new_capacity);
    if (!new_data) {
        serial_write_str("kbrlayout: OOM building updated config\n");
        minimafs_close(f);
        return false;
    }

    uint32_t out_pos = 0;
    bool wrote_key = false;
    size_t key_len = strlen(KBRLAYOUT_CONFIG_KEY);

    if (old_data && old_size > 0) {
        uint32_t line_start = 0;
        while (line_start < old_size) {
            uint32_t line_end = line_start;
            while (line_end < old_size && old_data[line_end] != '\n') line_end++;
            uint32_t line_len = line_end - line_start;
            bool has_nl = (line_end < old_size);

            bool is_key_line =
                (line_len >= key_len + 1) &&
                strncmp(old_data + line_start, KBRLAYOUT_CONFIG_KEY, key_len) == 0 &&
                old_data[line_start + key_len] == '=';

            if (!is_key_line) {
                if (out_pos + line_len + 2 > new_capacity) {
                    uint32_t grown = (out_pos + line_len + 2) * 2;
                    char* bigger = (char*)alloc_unzeroed(grown);
                    if (!bigger) { free_mem(new_data); minimafs_close(f); return false; }
                    memcpy(bigger, new_data, out_pos);
                    free_mem(new_data);
                    new_data = bigger;
                    new_capacity = grown;
                }
                memcpy(new_data + out_pos, old_data + line_start, line_len);
                out_pos += line_len;
                if (has_nl) new_data[out_pos++] = '\n';
            } else {
                wrote_key = true;
                uint32_t needed = (uint32_t)key_len + (uint32_t)strlen(value) + 3;
                if (out_pos + needed > new_capacity) {
                    uint32_t grown = (out_pos + needed) * 2;
                    char* bigger = (char*)alloc_unzeroed(grown);
                    if (!bigger) { free_mem(new_data); minimafs_close(f); return false; }
                    memcpy(bigger, new_data, out_pos);
                    free_mem(new_data);
                    new_data = bigger;
                    new_capacity = grown;
                }
                out_pos += (uint32_t)sprintf(new_data + out_pos, "%s=%s\n",
                                             KBRLAYOUT_CONFIG_KEY, value);
            }

            line_start = has_nl ? line_end + 1 : line_end;
        }
    }

    if (!wrote_key) {
        uint32_t needed = (uint32_t)key_len + (uint32_t)strlen(value) + 3;
        if (out_pos + needed > new_capacity) {
            uint32_t grown = (out_pos + needed) * 2;
            char* bigger = (char*)alloc_unzeroed(grown);
            if (!bigger) { free_mem(new_data); minimafs_close(f); return false; }
            memcpy(bigger, new_data, out_pos);
            free_mem(new_data);
            new_data = bigger;
            new_capacity = grown;
        }
        out_pos += (uint32_t)sprintf(new_data + out_pos, "%s=%s\n",
                                     KBRLAYOUT_CONFIG_KEY, value);
    }

    /* Swap in the rebuilt buffer; minimafs_close() will flush it. */
    if (f->data) free_mem(f->data);
    f->data      = (uint8_t*)new_data;
    f->data_size = out_pos;
    f->position  = out_pos;
    f->modified  = true;

    minimafs_close(f);

    serial_write_str("kbrlayout: persisted KeyboardLayout=");
    serial_write_str(value);
    serial_write_str("\n");
    return true;
}

/* ============================================================
 * list files under 0:/kbr (best-effort; silent if absent)
 * ============================================================ */

static void kbrlayout_list_files(void) {
    if (!minimafs_exists(KBRLAYOUT_KBR_DIR) || !minimafs_is_dir(KBRLAYOUT_KBR_DIR)) {
        return;
    }

    minimafs_dir_entry_t* entries =
        (minimafs_dir_entry_t*)alloc(64 * sizeof(minimafs_dir_entry_t));
    if (!entries) return;

    uint32_t count = minimafs_list_dir(KBRLAYOUT_KBR_DIR, entries, 64);
    if (count > 0) {
        graphics_write_textr("Files in ");
        graphics_write_textr(KBRLAYOUT_KBR_DIR);
        graphics_write_textr(":\n");
        for (uint32_t i = 0; i < count; i++) {
            if (entries[i].type == MINIMAFS_TYPE_DIR) continue;
            graphics_write_textr("  ");
            graphics_write_textr(KBRLAYOUT_KBR_DIR);
            graphics_write_textr("/");
            graphics_write_textr(entries[i].name);
            graphics_write_textr("\n");
        }
    }

    free_mem(entries);
}

/* ============================================================
 * Subcommand handlers
 * ============================================================ */

static void kbrlayout_do_get(void) {
    const keyboard_layout_t* layout = usb_keyboard_get_layout();

    if (!layout) {
        graphics_write_textr("No keyboard layout currently loaded\n");
    } else {
        graphics_write_textr("Active layout: ");
        graphics_write_textr(layout->header.name);
        graphics_write_textr(" (");
        graphics_write_textr(layout->header.language);
        graphics_write_textr(")\n");
    }

    char value[128];
    if (minimafs_get_config_value(KBRLAYOUT_CONFIG_KEY, KBRLAYOUT_CONFIG_PATH,
                                  value, sizeof(value))) {
        graphics_write_textr("Persisted setting (");
        graphics_write_textr(KBRLAYOUT_CONFIG_PATH);
        graphics_write_textr("): ");
        graphics_write_textr(KBRLAYOUT_CONFIG_KEY);
        graphics_write_textr("=");
        graphics_write_textr(value);
        graphics_write_textr("\n");
    }
}

static void kbrlayout_do_list(void) {
    graphics_write_textr("Built-in layouts:\n");
    graphics_write_textr("  US   - US QWERTY\n");
    graphics_write_textr("  SE   - Swedish QWERTY\n");
    graphics_write_textr("\n");
    kbrlayout_list_files();
    graphics_write_textr("\nUsage: kbrlayout set <US|SE|path.kbr>\n");
}

static void kbrlayout_do_set(const char* target) {
    if (!target || !*target) {
        graphics_write_textr("Usage: kbrlayout set <builtin name|.kbr path>\n");
        return;
    }

    /* Try a built-in layout by name first. */
    const keyboard_layout_t* builtin = usb_keyboard_get_builtin_layout(target);
    if (builtin) {
        if (!usb_keyboard_load_layout(builtin)) {
            graphics_write_textr("kbrlayout: failed to activate built-in layout\n");
            return;
        }

        graphics_write_textr("Loaded built-in layout: ");
        graphics_write_textr(target);
        graphics_write_textr("\n");

        if (!kbrlayout_persist_setting(target)) {
            graphics_write_textr("(note: could not persist to System.conf)\n");
        }
        return;
    }

    /* Otherwise treat it as a path to a .kbr file on MinimaFS. */
    if (!kbrlayout_load_from_file(target)) {
        graphics_write_textr("kbrlayout: failed to load '");
        graphics_write_textr(target);
        graphics_write_textr("' (not a built-in name and not a valid .kbr file)\n");
        return;
    }

    graphics_write_textr("Loaded layout from file: ");
    graphics_write_textr(target);
    graphics_write_textr("\n");

    if (!kbrlayout_persist_setting(target)) {
        graphics_write_textr("(note: could not persist to System.conf)\n");
    }
}

/* ============================================================
 * Command entry point
 * ============================================================ */

void cmd_kbrlayout(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: kbrlayout <get|set|list> [args]\n");
        return;
    }

    const char* sub = argv[1];

    if (strcmp(sub, "get") == 0) {
        kbrlayout_do_get();
    } else if (strcmp(sub, "list") == 0) {
        kbrlayout_do_list();
    } else if (strcmp(sub, "set") == 0) {
        kbrlayout_do_set(argc >= 3 ? argv[2] : NULL);
    } else {
        graphics_write_textr("kbrlayout: unknown subcommand '");
        graphics_write_textr(sub);
        graphics_write_textr("'. Use get|set|list\n");
    }
}

void register_kbrlayout(void) {
    command_register("kbrlayout", cmd_kbrlayout);
}

REGISTER_COMMAND(register_kbrlayout);
