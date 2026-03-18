#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "minimafshandler.h"

void cmd_initdisk(int argc, const char** argv) {
    initializeminimafs();
    ahci_drive_t* drive = getdrive(1);
    minimafs_disk_device_t* disk = setdrive(drive);
}

void register_initdisk(void) {
    command_register("initdisk", cmd_initdisk);
}

REGISTER_COMMAND(register_initdisk);
