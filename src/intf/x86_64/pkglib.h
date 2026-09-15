#ifndef PKGLIB_H
#define PKGLIB_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Library form of the .mpkg archive operations (see x86_64/pkgformat.h
 * for the on-disk layout). Unlike the `pkg`/`installpkg`/`pkginfo`
 * terminal commands (commands/pkgcommand.c), these take
 * already-resolved MinimaFS paths ("N:/...") and never touch the
 * shell's current-directory concept (fs_resolve_path()) - callers
 * here may be kernel code, a terminal command, or (via SYS_PKG in
 * syscall.c) an arbitrary user process with no shell of its own.
 *
 * None of these functions print to the terminal; callers that want
 * user-visible progress/errors do that themselves around the call.
 */

// Extracts every entry in the .mpkg archive at `archive_path` into
// `target_dir` (created if missing). On return, *out_installed and
// *out_failed (if non-NULL) hold the per-entry results. Returns false
// only for whole-archive failures (can't open/parse the archive,
// can't create the target directory) - individual entry failures are
// reported via *out_failed, not the return value.
bool pkglib_unzip(const char* archive_path, const char* target_dir,
                  uint32_t* out_installed, uint32_t* out_failed);

// Builds a .mpkg archive from `source_path` (a file or directory) and
// writes it to "<source_path-without-trailing-slash>.mpkg". If
// `out_path` is non-NULL, the resulting archive path is copied into
// it (truncated to fit `out_path_size`, always NUL-terminated if
// out_path_size > 0). `algorithm` is "lzss" (default - pass NULL for
// it) or "store"; any other value fails. Returns false on any error
// (bad path, OOM, unknown algorithm, write failure); partial output is
// cleaned up before returning.
bool pkglib_zip(const char* source_path, const char* algorithm,
                char* out_path, uint32_t out_path_size);

// Validates `archive_path` as a well-formed .mpkg archive and reports
// its entry count in *out_entry_count. Returns false if the archive
// can't be opened or is malformed.
bool pkglib_info(const char* archive_path, uint32_t* out_entry_count);

#endif // PKGLIB_H