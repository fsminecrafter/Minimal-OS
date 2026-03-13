#include <stdbool.h>
#include "print.h"
#include "x86_64/commandhandler.h"
#include "string.h"

extern void (*__start_command_ctors)(void);
extern void (*__stop_command_ctors)(void);

#define MAX_COMMANDS 4096

static struct CommandEntry commands[MAX_COMMANDS];
static int command_count = 0;

void commandhandler_init() {
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

    print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
    print_str("Unknown command: (");
    print_str(argv[0]);
    print_str(")\n");
}
