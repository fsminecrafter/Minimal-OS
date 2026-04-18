#include <stdbool.h>
#include <stdint.h>
#include "print.h"
#include "x86_64/commandhandler.h"
#include "string.h"
#include "graphics.h"
#include "serial.h"

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

void command_execute(const char* input) {
    serial_write_str("Executing...");
    // Tokenize
    static char buffer[256];
    strncpy(buffer, input, sizeof(buffer));
    buffer[sizeof(buffer)-1] = 0;

    char* argv[32] = { 0 };
    int argc = 0;
    char* tok = strtok(buffer, " ");
    while (tok && argc < 32) {
        argv[argc++] = tok;
        tok = strtok(NULL, " ");
    }

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
