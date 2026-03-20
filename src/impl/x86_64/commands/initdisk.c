#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "minimafshandler.h"

void cmd_initdisk(int argc, const char** argv) {
    initializeminimafs(0);
}

void register_initdisk(void) {
    command_register("initdisk", cmd_initdisk);
}

REGISTER_COMMAND(register_initdisk);
