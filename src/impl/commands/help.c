#include <stdbool.h>
#include "print.h"
#include "graphics.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "string.h"
#include "vgaterm.h"

static void help_section(const char* title) {
    graphics_write_textr("\n");
    graphics_write_textr(title);
    graphics_write_textr("\n");
}

static void help_item(const char* usage, const char* description) {
    graphics_write_textr("  ");
    graphics_write_textr(usage);
    graphics_write_textr("\n      ");
    graphics_write_textr(description);
    graphics_write_textr("\n");
}

static void help_general(void) {
    help_section("General");
    help_item("help [-c|--classic]", "Show paged help or the classic command list.");
    help_item("echo <text>", "Print text to the terminal.");
    help_item("clear", "Clear the terminal.");
    help_item("sleep <seconds|Nms>", "Pause for seconds or milliseconds.");
    help_item("systime", "Show the current system time.");
}

static void help_system_information(void) {
    help_section("System information");
    help_item("info", "Show system, CPU, PCI, and memory information.");
    help_item("cpu", "Show CPU information.");
    help_item("gpu", "Show GPU information.");
    help_item("memsize", "Show available memory in megabytes.");
    help_item("usage", "Show memory and process usage.");
    help_item("chgres <width> <height>", "Change the display resolution.");
}

static void help_files_and_directories(void) {
    help_section("Files and directories");
    help_item("list [path] [--recursive]", "List files and directories.");
    help_item("ls [path] [--recursive]", "Alias for list.");
    help_item("listroot", "Show the mounted drive root, including metadata.");
    help_item("lsroot", "Alias for listroot.");
    help_item("cd <path>", "Change the current directory.");
    help_item("mdr <path> [--hidden]", "Create a directory.");
    help_item("insert <file> <text>", "Create or replace a text file.");
    help_item("read <path> [--metadata]", "Read a file.");
    help_item("rd <path> [--metadata]", "Alias for read.");
    help_item("remove <path> [-r|--recursive]", "Remove a file or directory.");
    help_item("rm <path> [-r|--recursive]", "Alias for remove.");
    help_item("meta <option> <file>", "View or edit file metadata.");
}

static void help_storage_and_packages(void) {
    help_section("Storage and packages");
    help_item("initdisk [index]", "Detect and initialize a disk.");
    help_item("mount [index]", "Mount a MinimaFS disk.");
    help_item("format [index]", "Format a disk as MinimaFS.");
    help_item("importmusic", "Copy the built-in music file to the disk.");
    help_item("pkg <unzip|zip> ...", "Extract or create a package archive.");
    help_item("pkg unzip <archive> [target]", "Extract into an archive-named folder by default.");
    help_item("pkg zip <file|folder> [lzss|store]", "Create a package archive.");
    help_item("pkginfo <archive>", "Inspect a package archive.");
    help_item("installpkg <archive> [target]", "Alias for pkg unzip.");
}

static void help_audio(void) {
    help_section("Audio");
    help_item("play <file>", "Play an .adi audio file.");
    help_item("stop", "Stop audio playback.");
    help_item("pause", "Pause or resume audio playback.");
    help_item("volume <0-100>", "Set the audio volume.");
}

static void help_keyboard_and_programs(void) {
    help_section("Keyboard and programs");
    help_item("kbrlayout <set|get|list> [value]", "Manage the keyboard layout.");
    help_item("run <file>", "Run a .run executable.");
    help_item("services", "List registered services.");
}

static void help_diagnostics(void) {
    help_section("Diagnostics");
    help_item("testwrite", "Run the MinimaFS write test.");
    help_item("verifyfile", "Verify a file on disk.");
    help_item("diskusage", "Show disk usage.");
    help_item("trace_on", "Enable execution tracing.");
    help_item("trace_off", "Disable execution tracing.");
    help_item("trace_stack", "Show the execution trace stack.");
}

typedef void (*help_page_func_t)(void);

static bool help_wait_for_next_page(void) {
    graphics_write_textr("\nPress Enter for the next category, or q to exit.\n");
    while (true) {
        char key = vgaterm_wait_key();
        if (key == 'q' || key == 'Q') return false;
        if (key == '\n' || key == '\r') return true;
    }
}

void cmd_help(int argc, const char** argv) {
    if (argc >= 2 &&
        (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "--classic") == 0)) {
        command_list();
        return;
    }

    graphics_write_textr("MinimalOS terminal commands\n");
    graphics_write_textr("==========================\n");

    static const help_page_func_t pages[] = {
        help_general,
        help_system_information,
        help_files_and_directories,
        help_storage_and_packages,
        help_audio,
        help_keyboard_and_programs,
        help_diagnostics,
    };

    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        pages[i]();
        if (!help_wait_for_next_page()) {
            break;
        }
    }
}

void register_help(void) {
    command_register("help", cmd_help);
}

REGISTER_COMMAND(register_help);
