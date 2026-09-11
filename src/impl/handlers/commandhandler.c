#include <stdbool.h>
#include <stdint.h>
#include "print.h"
#include "x86_64/commandhandler.h"
#include "string.h"
#include "graphics.h"
#include "serial.h"
#include "prochandler.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/minimafs.h"
#include "fspaths.h"

extern void (*__start_command_ctors)(void);
extern void (*__stop_command_ctors)(void);

#define MAX_COMMANDS 512

static struct CommandEntry commands[MAX_COMMANDS];
static int command_count = 0;

void commandhandler_init() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    serial_write_str("Command handler init\n");

    uint64_t start = (uint64_t)&__start_command_ctors;
    uint64_t end   = (uint64_t)&__stop_command_ctors;

    serial_write_str("ctors start: ");
    serial_write_hex(start);
    serial_write_str("\n");

    serial_write_str("ctors end: ");
    serial_write_hex(end);
    serial_write_str("\n");

    if (start == end) {
        serial_write_str("WARNING: No command constructors found!\n");
        return;
    }

    int count = 0;

    for (void (**fn)() = &__start_command_ctors; fn < &__stop_command_ctors; ++fn) {
        serial_write_str("Calling ctor at: ");
        serial_write_hex((uint64_t)*fn);
        serial_write_str("\n");

        if (*fn == NULL) {
            serial_write_str("Skipping NULL ctor\n");
            continue;
        }

        (*fn)();
        count++;
    }

    serial_write_str("Total ctors executed: ");
    serial_write_hex(count);
    serial_write_str("\n");

    // Default file associations. Registered here (after every
    // REGISTER_COMMAND ctor above has run, so "run" is guaranteed to
    // already exist) rather than at REGISTER_COMMAND time in
    // runcommand.c, since command_register_extension() only stores a
    // name string - it doesn't need the target command to exist yet,
    // but keeping the registration next to command discovery makes
    // the startup ordering obvious rather than incidental.
    if (!command_register_extension("run", "run")) {
        serial_write_str("WARNING: failed to register default .run extension\n");
    }
}

void command_register(const char* name, command_func_t func) {
    if (command_count < MAX_COMMANDS) {
        commands[command_count++] = (struct CommandEntry){ name, func };
    }
}

// ===========================================
// FILE ASSOCIATIONS
// ===========================================

#define COMMAND_MAX_EXTENSIONS 32
#define COMMAND_EXT_MAX_LEN    15
#define COMMAND_ASSOC_CMD_LEN  32
#define COMMAND_ASSOC_REWRITE_BUF 320
#define COMMAND_ASSOC_MAX_DEPTH   4

typedef struct {
    char ext[COMMAND_EXT_MAX_LEN + 1];      // without leading '.', e.g. "run"
    char command[COMMAND_ASSOC_CMD_LEN];    // registered command name to dispatch to
    bool active;
} command_extension_assoc_t;

static command_extension_assoc_t g_extension_assocs[COMMAND_MAX_EXTENSIONS];
static int g_extension_assoc_count = 0;

// Guards against a misconfigured (e.g. self-referential) extension
// mapping turning command_execute()/command_execute_async() into
// unbounded recursion. In practice this never triggers - the
// associated command name ("run") has no '.' in it, so the recursive
// call's own association lookup bails out immediately (see
// command_resolve_file_association() below) - but future
// command_register_extension() callers could get creative, and this
// costs nothing to have in place.
static int g_assoc_depth = 0;

bool command_register_extension(const char* ext, const char* command) {
    if (!ext || !command || !*ext || !*command) return false;
    if (strlen(ext) > COMMAND_EXT_MAX_LEN) return false;
    if (strlen(command) >= COMMAND_ASSOC_CMD_LEN) return false;

    // Overwrite an existing mapping for the same extension if present.
    for (int i = 0; i < g_extension_assoc_count; i++) {
        if (g_extension_assocs[i].active && strcmp(g_extension_assocs[i].ext, ext) == 0) {
            strncpy(g_extension_assocs[i].command, command,
                    sizeof(g_extension_assocs[i].command) - 1);
            g_extension_assocs[i].command[sizeof(g_extension_assocs[i].command) - 1] = '\0';
            return true;
        }
    }

    if (g_extension_assoc_count >= COMMAND_MAX_EXTENSIONS) return false;

    command_extension_assoc_t* slot = &g_extension_assocs[g_extension_assoc_count++];
    strncpy(slot->ext, ext, COMMAND_EXT_MAX_LEN);
    slot->ext[COMMAND_EXT_MAX_LEN] = '\0';
    strncpy(slot->command, command, sizeof(slot->command) - 1);
    slot->command[sizeof(slot->command) - 1] = '\0';
    slot->active = true;
    return true;
}

// Case-insensitive extension comparison (".RUN" and ".run" should
// behave the same even though MinimaFS filenames themselves are
// case-sensitive).
static bool command_ext_equals(const char* a, const char* b) {
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

// Returns the extension (without the dot) of `path`, or NULL if there
// isn't one after the last path separator. Points into `path` itself -
// caller must not hold onto it past the lifetime of `path`.
static const char* command_file_extension(const char* path) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return NULL;  // no ext, ".hidden", or trailing dot
    return dot + 1;
}

// Resolves `argv0` (as typed - "./file.run", "0:/dir/file.run", or a
// bare "file.run") to a command name it should be dispatched to, or
// NULL if it isn't associated with anything. Only touches the
// filesystem at all if `argv0` looks like a filename (has an
// extension after the last path separator) - a plain unknown command
// name with no dot is left alone, so ordinary typos still get the
// normal "Unknown command" error instead of a filesystem lookup.
static const char* command_resolve_file_association(const char* argv0) {
    if (!argv0 || !*argv0) return NULL;

    const char* ext = command_file_extension(argv0);
    if (!ext) return NULL;

    char resolved[MINIMAFS_MAX_PATH];
    if (!fs_resolve_path(argv0, resolved)) return NULL;

    if (!minimafs_exists(resolved) || minimafs_is_dir(resolved)) {
        // Doesn't exist (or is a directory) - nothing to associate;
        // let the normal "unknown command" path report it.
        return NULL;
    }

    for (int i = 0; i < g_extension_assoc_count; i++) {
        if (g_extension_assocs[i].active && command_ext_equals(g_extension_assocs[i].ext, ext)) {
            return g_extension_assocs[i].command;
        }
    }

    // No extension mapping - fall back to the file's own metadata. A
    // file marked RUNNABLE (see `meta <file> executable true` in
    // metacommand.c) can be invoked directly even with an extension
    // nothing is explicitly registered for.
    minimafs_file_metadata_t metadata;
    if (minimafs_get_metadata(resolved, &metadata) && metadata.runnable) {
        // "run" is the only command that knows how to execute an
        // arbitrary loaded program today (see runcommand.c). If that
        // ever changes, metadata.run_with (already part of the file
        // format - see minimafs.h) is the natural place to record
        // which command a specific file should dispatch to instead of
        // hardcoding "run" here.
        return "run";
    }

    return NULL;
}

// Builds "<assoc_cmd> <original_input>" into `out`, bounded to
// `out_size`. Returns false (leaving `out` untouched) if it wouldn't
// fit, rather than silently truncating a command line.
static bool command_build_association_line(const char* assoc_cmd, const char* original_input,
                                            char* out, size_t out_size) {
    if (!assoc_cmd || !original_input || !out || out_size == 0) return false;
    int needed = snprintf(out, out_size, "%s %s", assoc_cmd, original_input);
    return needed > 0 && (size_t)needed < out_size;
}

// ===========================================
// TOKENIZER
// ===========================================
//
// Splits `buffer` in place (inserting NUL terminators as it goes) into
// up to `max_args` whitespace-separated tokens, with basic 'single' and
// "double" quote support. Returns the number of tokens found. Shared by
// both command_execute() and command_execute_async() so the two can
// never drift apart in how they parse a command line.
static int command_tokenize(char* buffer, char** argv, int max_args) {
    int argc = 0;
    char* cursor = buffer;

    while (*cursor && argc < max_args) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (!*cursor) break;

        argv[argc++] = cursor;
        char* output = cursor;
        bool quoted = false;
        char quote = '\0';

        while (*cursor) {
            if (quoted) {
                if (*cursor == quote) {
                    quoted = false;
                } else {
                    *output++ = *cursor;
                }
            } else if (*cursor == '"' || *cursor == '\'') {
                quoted = true;
                quote = *cursor;
            } else if (*cursor == ' ' || *cursor == '\t') {
                break;
            } else {
                *output++ = *cursor;
            }
            cursor++;
        }

        if (*cursor) cursor++;
        *output = '\0';
        while (*cursor == ' ' || *cursor == '\t') cursor++;
    }

    return argc;
}

void command_execute(const char* input) {
    serial_write_str("Executing...");
    // Tokenize
    static char buffer[256];
    strncpy(buffer, input, sizeof(buffer));
    buffer[sizeof(buffer)-1] = 0;

    char* argv[32] = { 0 };
    int argc = command_tokenize(buffer, argv, 32);

    if (argc == 0) return;

    // Lookup and call
    for (int i = 0; i < command_count; ++i) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].func(argc, (const char**)argv);
            return;
        }
    }

    // No direct command match - check for a file association (see
    // command_resolve_file_association()) before giving up, so
    // "./tool.run" behaves like "run ./tool.run".
    const char* assoc_cmd = command_resolve_file_association(argv[0]);
    if (assoc_cmd && g_assoc_depth < COMMAND_ASSOC_MAX_DEPTH) {
        char rewritten[COMMAND_ASSOC_REWRITE_BUF];
        if (command_build_association_line(assoc_cmd, input, rewritten, sizeof(rewritten))) {
            g_assoc_depth++;
            command_execute(rewritten);
            g_assoc_depth--;
            return;
        }
    }

    graphics_write_textr("Unknown command: (");
    graphics_write_textr(argv[0]);
    graphics_write_textr(")\n");
}

void command_list(void) {
    graphics_write_textr("Available commands:\n");

    for (int i = 0; i < command_count; i++) {
        graphics_write_textr("  ");
        graphics_write_textr(commands[i].name);
        graphics_write_textr("\n");
    }

    graphics_write_textr("\nTotal: ");
    
    char num[MAX_COMMANDS];
    itoa(command_count, num, 10);   // if you have itoa
    graphics_write_textr(num);
    
    graphics_write_textr(" commands\n");
}

// ===========================================
// PROCESS-BASED COMMAND EXECUTION
// ===========================================

#define COMMAND_PROC_INPUT_BUF 256
#define COMMAND_PROC_MAX_ARGS  32

typedef struct {
    char input_copy[COMMAND_PROC_INPUT_BUF];
    char* argv[COMMAND_PROC_MAX_ARGS];
    int argc;
    command_func_t func;
    bool valid;
} command_launch_ctx_t;

/*
 * Single-slot launch context. This is safe because command_execute_async()
 * refuses to launch a second command process while g_command_proc_pid is
 * still nonzero (see below) - the terminal only ever calls it again after
 * command_poll_running() has confirmed the previous command process is
 * fully gone, so at most one process is ever reading this struct at a
 * time.
 */
static command_launch_ctx_t g_launch_ctx;
static volatile uint64_t g_command_proc_pid = 0;

// Real entry point for a spawned command process. Reads the command that
// command_execute_async() already tokenized into g_launch_ctx before
// createProcess() was called.
static void command_proc_entry(void) {
    if (g_launch_ctx.valid && g_launch_ctx.func) {
        g_launch_ctx.func(g_launch_ctx.argc, (const char**)g_launch_ctx.argv);
    }
    g_launch_ctx.valid = false;
    process_exit();
}

uint64_t command_execute_async(const char* input) {
    if (!input) return 0;

    if (g_command_proc_pid != 0) {
        serial_write_str("command_execute_async: a command is already running\n");
        return 0;
    }

    strncpy(g_launch_ctx.input_copy, input, sizeof(g_launch_ctx.input_copy) - 1);
    g_launch_ctx.input_copy[sizeof(g_launch_ctx.input_copy) - 1] = '\0';

    int argc = command_tokenize(g_launch_ctx.input_copy, g_launch_ctx.argv,
                                 COMMAND_PROC_MAX_ARGS);
    if (argc == 0) return 0;

    command_func_t func = NULL;
    for (int i = 0; i < command_count; ++i) {
        if (strcmp(g_launch_ctx.argv[0], commands[i].name) == 0) {
            func = commands[i].func;
            break;
        }
    }

    if (!func) {
        // No direct command match - same file-association fallback as
        // command_execute(). Note: `input` here is the caller's
        // ORIGINAL string, not g_launch_ctx.input_copy - the tokenizer
        // above has already spliced NUL terminators into that copy, so
        // it can't be used to rebuild a full command line, but `input`
        // itself is untouched.
        const char* assoc_cmd = command_resolve_file_association(g_launch_ctx.argv[0]);
        if (assoc_cmd && g_assoc_depth < COMMAND_ASSOC_MAX_DEPTH) {
            char rewritten[COMMAND_ASSOC_REWRITE_BUF];
            if (command_build_association_line(assoc_cmd, input, rewritten, sizeof(rewritten))) {
                g_assoc_depth++;
                uint64_t pid = command_execute_async(rewritten);
                g_assoc_depth--;
                return pid;
            }
        }

        graphics_write_textr("Unknown command: (");
        graphics_write_textr(g_launch_ctx.argv[0]);
        graphics_write_textr(")\n");
        return 0;
    }

    g_launch_ctx.func  = func;
    g_launch_ctx.argc  = argc;
    g_launch_ctx.valid = true;

    process_t* proc = createProcess(g_launch_ctx.argv[0], command_proc_entry);
    if (!proc) {
        g_launch_ctx.valid = false;
        graphics_write_textr("Failed to start command process\n");
        return 0;
    }

    g_command_proc_pid = proc->pid;
    return proc->pid;
}

bool command_is_running(void) {
    return g_command_proc_pid != 0;
}

void command_poll_running(void) {
    if (g_command_proc_pid == 0) return;

    process_t* p = findProcessByPID(g_command_proc_pid);
    if (!p || p->state == PROCESS_ZOMBIE || p->state == PROCESS_TERMINATED) {
        g_command_proc_pid = 0;
    }
}

void command_kill_running(void) {
    if (g_command_proc_pid == 0) return;
    killProcess(g_command_proc_pid);
    /*
     * Don't clear g_command_proc_pid here. killProcess() only marks the
     * process ZOMBIE - the scheduler cleans it up (and, if it happened to
     * be the currently running process, actually switches away from it)
     * asynchronously. command_poll_running() clears the pid once that has
     * genuinely happened, so "is a command running" always tracks real
     * process state instead of a fire-and-forget request.
     */
}