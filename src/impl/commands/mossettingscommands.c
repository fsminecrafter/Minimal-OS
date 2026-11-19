/*
 * mossettings: system settings kept in 0:/etc/System.conf.
 *
 *     mossettings list                  every setting, its value and what it does
 *     mossettings get <name>            one value
 *     mossettings set <name> <value>    change it (saved at once)
 *
 * Names are matched without regard to case. Settings:
 *     bootupanim     true|false               the animation while the system starts
 *     bootanimstyle  logo|pulse|hourglass     which one (bootanim=<style> on the kernel line also works)
 *     nvidiaboot   true|false   the NVIDIA bring-up ('gpu nvidia boot') at every boot
 */
#include <stdbool.h>
#include <stddef.h>
#include "graphics.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/sysconf.h"

typedef enum { SET_BOOL, SET_CHOICE } setting_type_t;

typedef struct {
    const char* name;           // what the command takes
    const char* key;            // the System.conf key
    setting_type_t type;
    const char* fallback;       // the value when System.conf has none
    const char* help;
    const char* choices;        // SET_CHOICE: the values, separated by '|'
} setting_t;

static const setting_t k_settings[] = {
    { "bootupanim", "BootupAnim", SET_BOOL, "true",
      "The animation from power-on until the terminal or the desktop starts", NULL },
    { "bootanimstyle", "BootupAnimStyle", SET_CHOICE, "logo",
      "Which animation: the logo with circling dots, the logo breathing, or the turning hourglass",
      "logo|pulse|hourglass" },
    { "nvidiaboot", "NvidiaAutoBoot", SET_BOOL, "false",
      "Runs the NVIDIA bring-up ('gpu nvidia boot') at every boot; turned on when it first works", NULL },
};
#define SETTING_COUNT (sizeof(k_settings) / sizeof(k_settings[0]))

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool same(const char* a, const char* b) {
    while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return !*a && !*b;
}

static const setting_t* find(const char* name) {
    for (size_t i = 0; i < SETTING_COUNT; i++)
        if (same(name, k_settings[i].name) || same(name, k_settings[i].key)) return &k_settings[i];
    return NULL;
}

static bool pick_choice(const setting_t* s, const char* v, char* out, size_t cap);

static void value_of(const setting_t* s, char* out, size_t cap) {
    if (!sysconf_get(s->key, out, cap)) {
        strncpy(out, s->fallback, cap - 1);
        out[cap - 1] = 0;
    }
    if (s->type == SET_CHOICE) {
        char c[32];
        if (!pick_choice(s, out, c, sizeof(c))) { strncpy(out, s->fallback, cap - 1); out[cap - 1] = 0; }
        return;
    }
    if (s->type == SET_BOOL) {
        bool off = out[0] == 'f' || out[0] == 'F' || out[0] == '0' || out[0] == 'n' || out[0] == 'N';
        strncpy(out, off ? "false" : "true", cap - 1);
        out[cap - 1] = 0;
    }
}

// The choice in s->choices that v names (any case), copied to out; false if none.
static bool pick_choice(const setting_t* s, const char* v, char* out, size_t cap) {
    const char* p = s->choices;
    while (p && *p) {
        size_t n = 0;
        while (p[n] && p[n] != '|') n++;
        if (n + 1 <= cap) {
            memcpy(out, p, n);
            out[n] = 0;
            if (same(out, v)) return true;
        }
        p += n;
        if (*p == '|') p++;
    }
    return false;
}

// The value as stored, or NULL when it is not one the setting takes.
static const char* normalise(const setting_t* s, const char* v) {
    static char choice[32];
    if (s->type == SET_CHOICE) return pick_choice(s, v, choice, sizeof(choice)) ? choice : NULL;
    if (s->type == SET_BOOL) {
        if (same(v, "true") || same(v, "on") || same(v, "yes") || same(v, "1")) return "true";
        if (same(v, "false") || same(v, "off") || same(v, "no") || same(v, "0")) return "false";
    }
    return NULL;
}

static void usage(void) {
    graphics_write_textr("Usage: mossettings list\n"
                         "       mossettings get <name>\n"
                         "       mossettings set <name> <value>\n");
}

static void unknown(const char* name) {
    graphics_write_textr("mossettings: no setting called '");
    graphics_write_textr(name);
    graphics_write_textr("' (mossettings list shows them)\n");
}

void cmd_mossettings(int argc, const char** argv) {
    if (argc < 2) { usage(); return; }
    const char* sub = argv[1];
    char v[64];
    if (same(sub, "list")) {
        for (size_t i = 0; i < SETTING_COUNT; i++) {
            const setting_t* s = &k_settings[i];
            value_of(s, v, sizeof(v));
            graphics_write_textr(s->name);
            for (size_t k = strlen(s->name); k < 14; k++) graphics_write_textr(" ");
            graphics_write_textr(v);
            for (size_t k = strlen(v); k < 8; k++) graphics_write_textr(" ");
            if (s->type == SET_BOOL) graphics_write_textr("(true|false)  ");
            else { graphics_write_textr("("); graphics_write_textr(s->choices); graphics_write_textr(")  "); }
            graphics_write_textr(s->help);
            graphics_write_textr("\n");
        }
        return;
    }
    if (same(sub, "get")) {
        if (argc < 3) { usage(); return; }
        const setting_t* s = find(argv[2]);
        if (!s) { unknown(argv[2]); return; }
        value_of(s, v, sizeof(v));
        graphics_write_textr(v);
        graphics_write_textr("\n");
        return;
    }
    if (same(sub, "set")) {
        if (argc < 4) { usage(); return; }
        const setting_t* s = find(argv[2]);
        if (!s) { unknown(argv[2]); return; }
        const char* nv = normalise(s, argv[3]);
        if (!nv) {
            graphics_write_textr("mossettings: ");
            graphics_write_textr(s->name);
            if (s->type == SET_CHOICE) { graphics_write_textr(" takes one of "); graphics_write_textr(s->choices); graphics_write_textr("\n"); }
            else graphics_write_textr(" takes true or false\n");
            return;
        }
        if (!sysconf_set(s->key, nv)) {
            graphics_write_textr("mossettings: could not write 0:/etc/System.conf (is the disk mounted?)\n");
            return;
        }
        graphics_write_textr(s->name);
        graphics_write_textr(" = ");
        graphics_write_textr(nv);
        graphics_write_textr(" (from the next start)\n");
        return;
    }
    graphics_write_textr("mossettings: unknown subcommand '");
    graphics_write_textr(sub);
    graphics_write_textr("'\n");
    usage();
}

void register_mossettings(void) {
    command_register("mossettings", cmd_mossettings);
}

REGISTER_COMMAND(register_mossettings);
