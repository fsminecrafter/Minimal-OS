#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "minimafshandler.h"

void cmd_format(int argc, const char** argv) {
    minimafs_disk_device_t* device;
    device = getminimadrive();
    uint64_t size = 128ULL * 1024 * 1024;
    graphics_write_textr("Formatting...\n");
    minimafs_format(device, size, 1, "maindisk");
    graphics_write_textr("Formatting is complete check disk.\n");
}

void register_format(void) {
    command_register("format", cmd_format);
}

REGISTER_COMMAND(register_format);
