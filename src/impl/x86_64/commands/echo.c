#include "serial.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"

void cmd_echo(int argc, const char** argv) {
    terminal_t* terminal;
    terminal = graphics_get_terminal();
    for (int i = 1; i < argc; ++i) {
        graphics_write_textr(argv[i]);
        graphics_write_textr(" ");
    }
    graphics_write_textr("\n");
}

void register_echo() {
    command_register("echo", cmd_echo);
}

REGISTER_COMMAND(register_echo);