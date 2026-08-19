#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "time.h"
#include "string.h"

void cmd_systime(int argc, const char** argv) {
    string_t uptime;
    time_format_uptime(uptime.data, uptime.capacity);
    graphics_write_textr(uptime.data);
    graphics_write_textr("\n");
}

void register_systime(void) {
    command_register("systime", cmd_systime);
}

REGISTER_COMMAND(register_systime);
