/*
 * Path resolution shared by the terminal and the syscall layer.
 *
 * fs_resolve_path_from() is reentrant: it keeps no state of its own, so two
 * CPUs can resolve paths for two programs at the same time. Every program
 * that has its own working directory (SYS_CHDIR, SYS_SPAWN's cwd) resolves
 * against it; everyone else uses the terminal's (fs_resolve_path() in
 * diskcommands.c).
 *
 * Deliberately free of the kernel's string helpers so tests/host can build
 * it as is.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "fspaths.h"

static bool fsp_has_colon(const char* s) {
    for (; *s; s++) if (*s == ':') return true;
    return false;
}

// Appends `segment` (length len) to the normalized absolute local path.
static bool fsp_append(char* path, size_t* length, const char* segment, size_t len) {
    size_t need = *length + (*length > 1 ? 1 : 0) + len;
    if (need + 1 > MINIMAFS_MAX_PATH) return false;
    if (*length > 1) path[(*length)++] = '/';
    for (size_t i = 0; i < len; i++) path[(*length)++] = segment[i];
    path[*length] = '\0';
    return true;
}

static void fsp_pop(char* path, size_t* length) {
    if (*length <= 1) return;                   // "/" stays "/"
    while (*length > 1 && path[*length - 1] != '/') (*length)--;
    if (*length > 1) (*length)--;               // drop the separator too
    path[*length] = '\0';
}

// Applies the '/'-separated components of `local` to `path`.
static bool fsp_walk(char* path, size_t* length, const char* local) {
    const char* cursor = local;
    while (*cursor) {
        while (*cursor == '/') cursor++;
        const char* start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        size_t len = (size_t)(cursor - start);
        if (len == 0 || (len == 1 && start[0] == '.')) continue;
        if (len >= MINIMAFS_MAX_FILENAME) return false;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            fsp_pop(path, length);
            continue;
        }
        if (!fsp_append(path, length, start, len)) return false;
    }
    return true;
}

bool fs_resolve_path_from(const char* base, const char* input, char* resolved) {
    if (!base || !input || !resolved) return false;

    char local[MINIMAFS_MAX_PATH];
    char normalized[MINIMAFS_MAX_PATH];
    uint8_t drive_number = 0;
    size_t length = 1;
    normalized[0] = '/';
    normalized[1] = '\0';

    const char* effective = *input ? input : base;
    if (fsp_has_colon(effective)) {
        // "0:/dir/file" or "name:/dir/file": absolute on that drive.
        if (!minimafs_parse_path(effective, &drive_number, local)) return false;
        if (!fsp_walk(normalized, &length, local)) return false;
    } else {
        // "/dir/file" is absolute on the base's drive; anything else is
        // relative to the base directory.
        if (!minimafs_parse_path(base, &drive_number, local)) return false;
        if (effective[0] != '/' && !fsp_walk(normalized, &length, local)) return false;
        if (!fsp_walk(normalized, &length, effective)) return false;
    }

    // "<drive>:<normalized>"
    char digits[4];
    size_t nd = 0;
    uint8_t d = drive_number;
    do { digits[nd++] = (char)('0' + d % 10); d /= 10; } while (d && nd < sizeof(digits));
    if (nd + 1 + length + 1 > MINIMAFS_MAX_PATH) return false;
    size_t out = 0;
    while (nd) resolved[out++] = digits[--nd];
    resolved[out++] = ':';
    for (size_t i = 0; i <= length; i++) resolved[out++] = normalized[i];   // includes NUL
    return true;
}
