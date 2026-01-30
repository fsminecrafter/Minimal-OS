#include "print.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"

void cmd_echo(int argc, const char** argv) {
    for (int i = 1; i < argc; ++i) {
        print_str(argv[i]);
        print_str(" ");
    }
    print_str("\n");
}

void register_echo() {
    command_register("echo", cmd_echo);
}

REGISTER_COMMAND(register_echo);