#pragma once

typedef void (*command_func_t)(int argc, const char** argv);

struct CommandEntry {
    const char* name;
    command_func_t func;
};

// Entry point
void command_register(const char* name, command_func_t func);
void command_execute(const char* input);
void command_init();
void commandhandler_init();
void command_list(void);