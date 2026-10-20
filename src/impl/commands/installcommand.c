#include <stdbool.h>
#include <stdint.h>
#include "graphics.h"
#include "string.h"
#include "vgaterm.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/blockdev.h"
#include "x86_64/installer.h"
#include "minimafshandler.h"

/*
 * install                                   show disks and usage
 * install <disk> [--mbr] [--size <N[M|G|T]>] [--label <name>] [--yes]
 * install --system <part> --boot <part> [--yes]
 */

static void say(const char* s) { graphics_write_textr(s); }

static bool parse_size(const char* s, uint64_t* bytes) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    const char* p = s;
    while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
    if (p == s) return false;
    uint64_t mul = 1024ULL * 1024;
    if (*p == 'G' || *p == 'g') mul = 1024ULL * 1024 * 1024;
    else if (*p == 'T' || *p == 't') mul = 1024ULL * 1024 * 1024 * 1024;
    else if (*p == 'M' || *p == 'm' || *p == '\0') mul = 1024ULL * 1024;
    else return false;
    *bytes = v * mul;
    return true;
}

static uint32_t g_last_percent;

static void print_progress(uint32_t percent, const char* text) {
    char line[128];
    if (percent < g_last_percent) percent = g_last_percent;
    g_last_percent = percent;
    snprintf(line, sizeof(line), "[%3u%%] %s\n", percent, text);
    say(line);
}

static void usage(void) {
    say("usage: install <disk> [--mbr] [--size 20G] [--label name] [--yes]\n"
        "       install --system <partition> --boot <partition> [--yes]\n"
        "The first form erases the whole disk. The disk then boots on both\n"
        "BIOS and UEFI PCs. See 'disks' for names.\n");
}

static bool confirm(const char* what) {
    say("This will ERASE ");
    say(what);
    say(". Type y to continue: ");
    char key = vgaterm_wait_key();
    char echo[3] = { key, '\n', 0 };
    say(echo);
    return key == 'y' || key == 'Y';
}

void cmd_install(int argc, const char** argv) {
    install_options_t opt;
    memset(&opt, 0, sizeof(opt));
    bool yes = false;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--yes") == 0 || strcmp(a, "-y") == 0) yes = true;
        else if (strcmp(a, "--mbr") == 0) opt.use_mbr = true;
        else if (strcmp(a, "--gpt") == 0) opt.use_mbr = false;
        else if (strcmp(a, "--size") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &opt.system_size)) { say("install: bad size\n"); return; }
        } else if (strcmp(a, "--label") == 0 && i + 1 < argc) opt.label = argv[++i];
        else if (strcmp(a, "--system") == 0 && i + 1 < argc) opt.system_part = argv[++i];
        else if (strcmp(a, "--boot") == 0 && i + 1 < argc) opt.boot_part = argv[++i];
        else if (a[0] != '-' && !opt.disk) opt.disk = a;
        else { usage(); return; }
    }

    initializeminimafs(0);
    if (!opt.disk && !opt.system_part) {
        usage();
        say("\nDisks:\n");
        command_execute("disks");
        if (!installer_payload_available()) {
            say("\nNote: no install payload loaded. Boot from the Minimal-OS ISO\n"
                "(or an installed system) to install.\n");
        }
        return;
    }
    if (opt.disk && (opt.system_part || opt.boot_part)) {
        usage();
        return;
    }

    char what[96];
    if (opt.disk) snprintf(what, sizeof(what), "everything on %s", opt.disk);
    else snprintf(what, sizeof(what), "%s and %s", opt.system_part, opt.boot_part);
    if (!yes && !confirm(what)) {
        say("Cancelled.\n");
        return;
    }

    g_last_percent = 0;
    char err[128];
    if (installer_run(&opt, print_progress, err, sizeof(err))) {
        if (err[0]) {
            say("Warning: ");
            say(err);
            say("\n");
        }
        say("Minimal-OS is installed.\n");
    } else {
        say("Install failed: ");
        say(err);
        say("\n");
    }
}

static void register_install(void) {
    command_register("install", cmd_install);
}

REGISTER_COMMAND(register_install);
