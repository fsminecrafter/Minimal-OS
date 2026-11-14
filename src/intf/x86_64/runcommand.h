#ifndef RUNCOMMAND_H
#define RUNCOMMAND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
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
// The same, with the new program's pid (see run_launch_t::out_pid).
process_t* run_launch_file_pid(const char* run_path, int extra_argc, const char** extra_argv,
                               uint64_t* out_pid);

typedef struct stdio_sink stdio_sink_t;

// Everything run_launch_ex() can set up for a new program.
typedef struct {
    const char* path;             // fully qualified: a .run bundle or an ELF file
    uint32_t argc;                // argv exactly as the program sees it;
    const char* const* argv;      // argc 0 means { path }
    const char* env;              // packed "A=1\0B=2\0" (copied), or NULL
    uint32_t env_len;
    const char* cwd;              // working directory (copied); NULL = terminal's
    stdio_sink_t* out_sink;       // output sinks; the references move to the
    stdio_sink_t* err_sink;       // program, and are dropped if launching fails
    uint64_t parent_pid;          // 0 = the calling process
    bool wait_record;             // its parent may SYS_WAIT for it
    bool terminal_foreground;     // the terminal treats it as the program it runs
    uint64_t stack_size;          // main stack bytes; 0 = the default (8 MiB)
    // Receives the new program's pid before it starts. Use this, not the
    // returned pointer: the program may run, end and be freed on another CPU
    // before the launcher gets its next instruction (the pointer only says
    // "it started").
    uint64_t* out_pid;
} run_launch_t;

// The general form of run_launch_file(), used by SYS_SPAWN and SYS_EXEC.
process_t* run_launch_ex(const run_launch_t* launch);

#endif // RUNCOMMAND_H
