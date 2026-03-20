#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "x86_64/minimafs.h"
#include "minimafshandler.h"
#include <stdbool.h>

void cmd_mount(int argc, const char** argv) {
    minimafs_disk_device_t* device;
    device = getminimadrive();
    int success = mountdrive(device, 0);
    if (success == 1) {
        graphics_write_textr("Mount succeded.\n");
    }else if (success == 2) {
        graphics_write_textr("MinimaFS: Drive already mounted\n");
    }else if (success == 3) {
        graphics_write_textr("MinimaFS: Failed to parse storage.desc\n");
    }else if (success == 4) {
        graphics_write_textr("MinimaFS: Invalid root block\n");
    }else if (success == 5) {
        graphics_write_textr("MinimaFS: Drive too large for bitmap\n");
    } else {
        graphics_write_textr("Mount failed.\n");
    }

    serial_write_str("DEBUG: listing drive 0\n");
    minimafs_drive_t* d = get_drive(0);

    if (!d) {
        serial_write_str("Drive 0 = NULL\n");
    } else if (!d->mounted) {
        serial_write_str("Drive 0 not mounted\n");
    } else {
        serial_write_str("Drive 0 OK\n");
    }

}
void register_mount(void) {
    command_register("mount", cmd_mount);
}

REGISTER_COMMAND(register_mount);
