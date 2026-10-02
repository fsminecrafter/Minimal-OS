#include "graphics.h"
#include "serial.h"
#include "x86_64/acpi.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"

static void cmd_shutdown(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    graphics_write_textr("Requesting ACPI shutdown...\n");
    serial_write_str("Power: shutdown requested\n");
    if (!acpi_shutdown()) {
        graphics_write_textr("Shutdown unavailable on this machine.\n");
    }
}

static void cmd_reboot(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    graphics_write_textr("Rebooting...\n");
    serial_write_str("Power: reboot requested\n");
    if (!acpi_reboot()) {
        graphics_write_textr("Reboot failed.\n");
    }
}

static void register_power_commands(void) {
    command_register("shutdown", cmd_shutdown);
    command_register("reboot", cmd_reboot);
}

REGISTER_COMMAND(register_power_commands);