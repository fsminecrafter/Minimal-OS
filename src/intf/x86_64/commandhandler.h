#pragma once

#include <stdint.h>
#include <stdbool.h>

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

// ===========================================
// PROCESS-BASED COMMAND EXECUTION
// ===========================================
//
// Runs a full command line ("format 0", "importmusic", ...) as its own
// kernel process instead of executing it inline on the caller's stack.
// Only one command process may be in flight at a time.

// Launches `input` as a new process. Returns the new process's PID, or 0
// if nothing was launched (unknown command, empty input, allocation
// failure, or a command process is already running - see
// command_is_running()).
uint64_t command_execute_async(const char* input);

// True if a command process launched via command_execute_async() is
// still running, as of the last command_poll_running() call.
bool command_is_running(void);

// Call periodically (e.g. once per terminal tick) to notice when the
// in-flight command process has terminated and clear the "running"
// state. Safe to call even when no command is running.
void command_poll_running(void);

// Kills the currently-running command process, if any (e.g. on Ctrl+C).
// No-op if no command is running.
void command_kill_running(void);
