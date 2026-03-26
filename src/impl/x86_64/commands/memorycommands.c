#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "x86_64/globaldatatable.h"
#include "serial.h"

void cmd_memsize(int argc, const char** argv) {
    serial_write_str("Executing...");
    uint32_t totalrambytes = findvar("totalrambytes");
    char totalramstringkilobytes[128];
    char totalramstringmib[64];
    serial_write_str("Converting...");
    int_to_str(totalrambytes / 1024, totalramstringkilobytes);
    int_to_str(totalrambytes / 1048576, totalramstringmib);
    graphics_write_textr(totalramstringkilobytes);
    graphics_write_textr("KiB");
    graphics_write_textr(" || ");
    graphics_write_textr(totalramstringmib);
    graphics_write_textr("MiB");
    graphics_write_textr("\n");
}

void register_memsize(void) {
    command_register("memsize", cmd_memsize);
}

REGISTER_COMMAND(register_memsize);
