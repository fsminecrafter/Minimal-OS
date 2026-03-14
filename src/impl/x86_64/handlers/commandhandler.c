#include <stdbool.h>
#include "print.h"
#include "x86_64/commandhandler.h"
#include "string.h"
#include "graphics.h"

extern void (*__start_command_ctors)(void);
extern void (*__stop_command_ctors)(void);

#define MAX_COMMANDS 4096

static struct CommandEntry commands[MAX_COMMANDS];
static int command_count = 0;

void commandhandler_init() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    for (void (**fn)() = &__start_command_ctors; fn < &__stop_command_ctors; ++fn) {
        (*fn)();
    }
}
void command_register(const char* name, command_func_t func) {
    if (command_count < MAX_COMMANDS) {
        commands[command_count++] = (struct CommandEntry){ name, func };
    }
}

void command_execute(const char* input) {
    // Tokenize
    static char buffer[128];
    strncpy(buffer, input, sizeof(buffer));
    buffer[sizeof(buffer)-1] = 0;

    char* argv[16] = { 0 };
    int argc = 0;
    char* tok = strtok(buffer, " ");
    while (tok && argc < 16) {
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
    
    char num[16];
    itoa(command_count, num, 10);   // if you have itoa
    graphics_write_textr(num);
    
    graphics_write_textr(" commands\n");
}
