#include <stdbool.h>
#include <stdint.h>
#include "print.h"
#include "x86_64/commandhandler.h"
#include "string.h"
#include "graphics.h"
#include "serial.h"
#include "prochandler.h"
#include "x86_64/proc.h"

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
}

void command_register(const char* name, command_func_t func) {
    if (command_count < MAX_COMMANDS) {
        commands[command_count++] = (struct CommandEntry){ name, func };
    }
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
