#ifndef RUNCOMMAND_H
#define RUNCOMMAND_H

#include <stddef.h>
#include "x86_64/proc.h"

/*
 * Shared helpers behind the `run` command (runcommand.c), exposed so
 * other subsystems - currently the services manager - can launch a
 * .run bundle the exact same way `run <path>` does, without going
 * through the terminal's command dispatcher.
 */

// Normalizes `input` into a fully-qualified MinimaFS path: "./x" and a
// bare "x" become "0:/x"; anything that already has a drive prefix
// ("N:/...") is left as-is. `normalized` must point at a buffer of at
// least MINIMAFS_MAX_PATH bytes. Returns `normalized` on success, or
// NULL if the result wouldn't fit.
const char* run_normalize_path(const char* input, char* normalized, size_t normalized_size);

// Loads the .run bundle (MINIRUN1 archive) at `run_path` - which must
// already be a fully-qualified path, see run_normalize_path() - and
// starts it as a new user process, exactly like the `run` command.
//
// `extra_argc`/`extra_argv` are additional arguments handed to the
// program as argv[1..] (argv[0] is always `run_path` itself) - e.g.
// the "-h --hello 0:/randomfile" in "run 0:/hello.run -h --hello
// 0:/randomfile", or equivalently "./hello.run -h --hello
// 0:/randomfile" typed directly at the prompt (see
// command_resolve_file_association() in commandhandler.c, which
// forwards the whole original line through to `run`). Pass 0/NULL for
// a program that takes no arguments.
//
// Returns the new process, or NULL on failure (bad path, malformed
// archive/ELF, too many/too-long arguments, or OOM).
process_t* run_launch_file(const char* run_path, int extra_argc, const char** extra_argv);

#endif // RUNCOMMAND_H
