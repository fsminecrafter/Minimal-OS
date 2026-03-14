#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"

void cmd_help(int argc, const char** argv) {
    command_list();
}

void register_help(void) {
    command_register("help", cmd_help);
}

REGISTER_COMMAND(register_help);
