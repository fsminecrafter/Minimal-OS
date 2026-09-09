#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "graphics.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/scheduler.h"

// ===========================================
// SLEEP COMMAND
// ===========================================
//
// Usage: sleep <seconds>     (e.g. "sleep 3")
//        sleep <N>ms         (e.g. "sleep 500ms")
//
// Runs as its own command process, via command_execute_async() in
// commandhandler.c, so it can be safely cancelled with Ctrl+C:
// terminal_keyboard_callback() routes Ctrl+C to command_kill_running(),
// which calls killProcess() and marks this process's process_t as
// PROCESS_ZOMBIE.
//
// sleep() (scheduler.h) parks the calling process in PROCESS_WAITING
// via a single schedule() call rather than busy-looping - it does not
// re-check its own state once asleep. A process killed while
// PROCESS_WAITING is simply never picked up by
// wake_sleeping_processes() (which only wakes PROCESS_WAITING
// processes; ZOMBIE is excluded) and instead gets reaped - its
// process_t and kernel stack freed - by the next schedule() cleanup
// pass, since that pass only skips over ZOMBIE/TERMINATED entries when
// they happen to *be* current_process. Because a WAITING process is
// never current_process while it's asleep, this reap can and does
// happen while this command process is still logically "parked" in the
// middle of its own sleep() call - but since it's ZOMBIE, the scheduler
// will never schedule it back in, so its stack (and this function) will
// simply never execute again. That is the exact same mechanism every
// other sleeping process in this kernel already relies on when killed
// (see killProcess() / schedule() in scheduler.c) - no additional
// cancellation plumbing is needed here, and none is added, since adding
// a manual "wake up and check a cancel flag" loop would just reintroduce
// a race against the same kill path this already handles safely.
void cmd_sleep(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: sleep <seconds>  or  sleep <N>ms\n");
        return;
    }

    const char* arg = argv[1];
    size_t len = strlen(arg);
    bool is_ms = (len > 2 && arg[len - 2] == 'm' && arg[len - 1] == 's');

    uint64_t value = 0;
    const char* p = arg;
    while (*p == ' ' || *p == '\t') p++;

    bool saw_digit = false;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint64_t)(*p - '0');
        p++;
        saw_digit = true;
    }

    if (!saw_digit) {
        graphics_write_textr("sleep: invalid duration '");
        graphics_write_textr(arg);
        graphics_write_textr("'\n");
        return;
    }

    uint64_t ms = is_ms ? value : value * 1000ULL;

    // Guard against a pathologically large request (typo'd extra digit,
    // etc.) turning into an effectively-unbounded sleep. One hour is far
    // beyond any reasonable interactive use of this command, and it can
    // always be cut short with Ctrl+C regardless.
    const uint64_t MAX_SLEEP_MS = 3600ULL * 1000ULL;
    if (ms > MAX_SLEEP_MS) {
        graphics_write_textr("sleep: duration clamped to 1 hour\n");
        ms = MAX_SLEEP_MS;
    }

    if (ms == 0) return;

    sleep(ms);
}

void register_sleep(void) {
    command_register("sleep", cmd_sleep);
}

REGISTER_COMMAND(register_sleep);