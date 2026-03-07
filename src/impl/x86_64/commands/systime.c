#include <stdbool.h>
#include "print.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "time.h"

void cmd_systime(int argc, const char** argv) {
    time_print_datetime(true);
    print_str("\n");
}

void register_systime(void) {
    command_register("systime", cmd_systime);
}

REGISTER_COMMAND(register_systime);
