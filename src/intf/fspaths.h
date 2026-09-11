#ifndef FSPATHS_H
#define FSPATHS_H

#include <stdbool.h>
#include "x86_64/minimafs.h"

/*
 * Shared path-resolution helpers for the command system.
 *
 * fs_resolve_path() and fs_get_current_directory() are both defined in
 * diskcommands.c (alongside 'cd', which is what actually changes the
 * directory they resolve relative to), but every command that accepts
 * a path argument needs to agree on what a relative path means - so
 * this header exists purely to let other command files, and the
 * command dispatcher itself (commandhandler.c's file-association
 * fallback), call into that same logic instead of reinventing their
 * own (weaker) normalization.
 */

// Resolves `input` into a fully qualified "N:/local/path" MinimaFS
// path, honoring the shell's current directory (see
// fs_get_current_directory()) for relative input and normalizing "."
// and ".." components. `resolved` must point at a buffer of at least
// MINIMAFS_MAX_PATH bytes. Returns false if `input` can't be resolved
// (e.g. path too long, or references an unknown named drive).
bool fs_resolve_path(const char* input, char* resolved);

// Returns the shell's current working directory, e.g. "0:/etc". Never
// returns NULL - defaults to "0:/".
const char* fs_get_current_directory(void);

#endif // FSPATHS_H