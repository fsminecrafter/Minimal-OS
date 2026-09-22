#include "x86_64/servicemanager.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/runcommand.h"
#include "x86_64/scheduler.h"
#include "x86_64/thread.h"
#include "prochandler.h"
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include <stdint.h>
#include <stdbool.h>

#define SERVICES_DIR       "0:/services"

#define SERVICE_MAX_COUNT  32
#define SERVICE_MAX_NAME   64     // matches MINIMAFS_MAX_ENTRY_NAME
#define SERVICE_MAX_DESC   128
#define SERVICE_MAX_CMD    256
#define SERVICE_LINE_MAX   320

typedef enum {
    SERVICE_EXIT_NONE = 0,
    SERVICE_EXIT_RESTART,
    SERVICE_EXIT_SCRIPT,
} service_exit_mode_t;

typedef struct {
    char filename[SERVICE_MAX_NAME];      // e.g. "myservice.service" - also the
                                           // key used to match a monitor process
                                           // back to its own config (see
                                           // service_find_by_process_name()).
    char name[SERVICE_MAX_NAME];          // [GENERAL] name
    char desc[SERVICE_MAX_DESC];          // [GENERAL] desc

    service_exit_mode_t exit_mode;        // [GENERAL] exit
    char exitscript[SERVICE_MAX_CMD];     // [GENERAL] exitscript

    bool has_exec;
    char exec_path[SERVICE_MAX_CMD];     // [STARTUP] exec path or command
    bool startup_ran;                    // command-style startup already ran

    bool has_script;
    char startup_script[SERVICE_MAX_CMD]; // [STARTUP] script

    bool active;                          // slot holds a registered service
} service_entry_t;

static service_entry_t g_services[SERVICE_MAX_COUNT];
static int g_service_count = 0;

/* ===========================================
 * Small string helpers
 * =========================================== */

static bool service_streq_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool service_has_extension(const char* filename, const char* ext) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename || !dot[1]) return false;
    return service_streq_ci(dot + 1, ext);
}

static void service_trim(char* s) {
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

// Takes only the first comma/whitespace-delimited token, so a value
// like "None, Restart, Script" (documenting the valid options) parses
// as "None" rather than accidentally matching "Restart".
static service_exit_mode_t service_parse_exit_mode(const char* value) {
    char token[16];
    size_t i = 0;
    while (value[i] && value[i] != ',' && value[i] != ' ' && i + 1 < sizeof(token)) {
        token[i] = to_lower(value[i]);
        i++;
    }
    token[i] = '\0';

    if (strcmp(token, "restart") == 0) return SERVICE_EXIT_RESTART;
    if (strcmp(token, "script")  == 0) return SERVICE_EXIT_SCRIPT;
    return SERVICE_EXIT_NONE;
}

// Parses "key=value" (value may be "quoted" and/or trail off into a
// // comment). Returns false if there's no '=' on the line.
static bool service_parse_kv(const char* line, char* key, size_t key_size,
                             char* value, size_t value_size) {
    const char* eq = strchr(line, '=');
    if (!eq) return false;

    size_t klen = (size_t)(eq - line);
    if (klen == 0 || klen >= key_size) return false;
    memcpy(key, line, klen);
    key[klen] = '\0';
    service_trim(key);

    char raw[SERVICE_MAX_CMD];
    size_t rlen = 0;
    bool in_quotes = false;
    for (const char* v = eq + 1; *v && rlen + 1 < sizeof(raw); v++) {
        if (*v == '"') { in_quotes = !in_quotes; continue; }
        if (!in_quotes && v[0] == '/' && v[1] == '/') break;
        raw[rlen++] = *v;
    }
    raw[rlen] = '\0';
    service_trim(raw);

    strncpy(value, raw, value_size - 1);
    value[value_size - 1] = '\0';
    return true;
}

/* ===========================================
 * .service file parsing
 * =========================================== */

static bool service_parse_file(const char* path, service_entry_t* out) {
    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) return false;

    uint32_t size = minimafs_size(f);
    if (size == 0) { minimafs_close(f); return false; }

    char* buf = (char*)alloc_unzeroed(size + 1);
    if (!buf) { minimafs_close(f); return false; }

    uint32_t got = minimafs_read(f, buf, size);
    minimafs_close(f);
    buf[got] = '\0';

    memset(out, 0, sizeof(*out));  // exit_mode = SERVICE_EXIT_NONE, has_* = false

    enum { SEC_NONE, SEC_GENERAL, SEC_STARTUP } section = SEC_NONE;

    char* line = buf;
    while (line) {
        char* line_end = strchr(line, '\n');
        size_t len = line_end ? (size_t)(line_end - line) : strlen(line);

        char linebuf[SERVICE_LINE_MAX];
        if (len >= sizeof(linebuf)) len = sizeof(linebuf) - 1;
        memcpy(linebuf, line, len);
        linebuf[len] = '\0';
        service_trim(linebuf);

        char* next_line = line_end ? line_end + 1 : NULL;

        if (linebuf[0] == '\0' ||
            (linebuf[0] == '/' && linebuf[1] == '/')) {
            line = next_line;
            continue;
        }

        if (linebuf[0] == '[') {
            char* close = strchr(linebuf, ']');
            if (close) {
                *close = '\0';
                const char* sec_name = linebuf + 1;
                if (service_streq_ci(sec_name, "GENERAL"))      section = SEC_GENERAL;
                else if (service_streq_ci(sec_name, "STARTUP")) section = SEC_STARTUP;
                else                                            section = SEC_NONE;
            }
            line = next_line;
            continue;
        }

        char key[32];
        char value[SERVICE_MAX_CMD];
        if (service_parse_kv(linebuf, key, sizeof(key), value, sizeof(value))) {
            if (section == SEC_GENERAL) {
                if (service_streq_ci(key, "name")) {
                    strncpy(out->name, value, sizeof(out->name) - 1);
                } else if (service_streq_ci(key, "desc")) {
                    strncpy(out->desc, value, sizeof(out->desc) - 1);
                } else if (service_streq_ci(key, "exit")) {
                    out->exit_mode = service_parse_exit_mode(value);
                } else if (service_streq_ci(key, "exitscript")) {
                    strncpy(out->exitscript, value, sizeof(out->exitscript) - 1);
                }
            } else if (section == SEC_STARTUP) {
                if (service_streq_ci(key, "script")) {
                    strncpy(out->startup_script, value, sizeof(out->startup_script) - 1);
                    out->has_script = true;
                } else if (service_streq_ci(key, "exec")) {
                    strncpy(out->exec_path, value, sizeof(out->exec_path) - 1);
                    out->has_exec = true;
                }
            }
        }

        line = next_line;
    }

    free_mem(buf);
    return true;
}

/* ===========================================
 * Launching / monitoring
 * =========================================== */

static const char* service_display_name(service_entry_t* svc) {
    return svc->name[0] ? svc->name : svc->filename;
}

// If `script` is exactly "run <path>", extracts <path> into `out` and
// returns true - that's the one script form we can actually launch
// and monitor a pid for, same as [STARTUP] exec.
static bool service_script_run_path(const char* script, char* out, size_t out_size) {
    const char* p = script;
    while (*p == ' ' || *p == '\t') p++;

    const char* prefix = "run ";
    size_t plen = strlen(prefix);
    if (strncmp(p, prefix, plen) != 0) return false;
    p += plen;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return false;

    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\t' && i + 1 < out_size) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

// Launches the service once and blocks until a directly launched .run exits.
// Command-style exec/script entries are handed to the command system once;
// those commands do not provide a process pid for service monitoring.
static void service_run_and_wait(service_entry_t* svc) {
    process_t* proc = NULL;
    char target_path[MINIMAFS_MAX_PATH];
    const char* raw_path = NULL;

    if (svc->startup_ran) return;

    if (svc->has_exec) {
        char run_arg[MINIMAFS_MAX_PATH];
        if (service_script_run_path(svc->exec_path, run_arg, sizeof(run_arg))) {
            const char* resolved = run_normalize_path(run_arg, target_path, sizeof(target_path));
            if (resolved) raw_path = target_path;
        } else if (svc->exec_path[0] == '/' ||
                   svc->exec_path[0] == '.' || strchr(svc->exec_path, ':')) {
            raw_path = svc->exec_path;
        } else {
            serial_write_str("[SERVICES] '");
            serial_write_str(service_display_name(svc));
            serial_write_str("' executing command: ");
            serial_write_str(svc->exec_path);
            serial_write_str("\n");
            command_execute(svc->exec_path);
            return;
        }
    } else if (svc->has_script) {
        char run_arg[MINIMAFS_MAX_PATH];
        if (service_script_run_path(svc->startup_script, run_arg, sizeof(run_arg))) {
            char normalized[MINIMAFS_MAX_PATH];
            const char* resolved = run_normalize_path(run_arg, normalized, sizeof(normalized));
            if (resolved) {
                strncpy(target_path, resolved, sizeof(target_path) - 1);
                target_path[sizeof(target_path) - 1] = '\0';
                raw_path = target_path;
            }
        } else {
            serial_write_str("[SERVICES] '");
            serial_write_str(service_display_name(svc));
            serial_write_str("': script is not a 'run <path>' command, "
                              "running once with no exit tracking\n");
            command_execute(svc->startup_script);
            return;
        }
    }

    if (raw_path) {
        char normalized[MINIMAFS_MAX_PATH];
        const char* resolved = run_normalize_path(raw_path, normalized, sizeof(normalized));
        if (resolved) {
            // Services launched this way take no arguments today.
            proc = run_launch_file(resolved, 0, NULL);
        }
    }

    if (!proc) {
        serial_write_str("[SERVICES] '");
        serial_write_str(service_display_name(svc));
        serial_write_str("': failed to start\n");
        return;
    }

    serial_write_str("[SERVICES] '");
    serial_write_str(service_display_name(svc));
    serial_write_str("' started (PID ");
    serial_write_dec(proc->pid);
    serial_write_str(")\n");

    thread_join(proc->pid);

    serial_write_str("[SERVICES] '");
    serial_write_str(service_display_name(svc));
    serial_write_str("' exited\n");
}

// Every monitor process shares this one entry function; it identifies
// which service it's responsible for by matching its OWN process name
// (set to svc->filename at creation time - see createProcess() below)
// against the registered service table. This avoids passing per-service
// context through any shared/global scratch state, which would race
// once more than one monitor process can be scheduled concurrently.
static service_entry_t* service_find_by_process_name(void) {
    const char* proc_name = getCurrentProcessName();
    for (int i = 0; i < g_service_count; i++) {
        if (!g_services[i].active) continue;
        size_t len = strlen(g_services[i].filename);
        if (strncmp(proc_name, g_services[i].filename, len) == 0 &&
            proc_name[len] == '@') {
            return &g_services[i];
        }
    }
    return NULL;
}

static void service_monitor_entry(void) {
    service_entry_t* svc = service_find_by_process_name();
    if (!svc) {
        serial_write_str("[SERVICES] monitor: could not identify its own service\n");
        return;
    }

    for (;;) {
        service_run_and_wait(svc);

        if (svc->exit_mode == SERVICE_EXIT_RESTART) {
            serial_write_str("[SERVICES] '");
            serial_write_str(service_display_name(svc));
            serial_write_str("' restarting...\n");
            sleep(1000);  // brief backoff so a crash loop doesn't spin
            continue;
        }

        if (svc->exit_mode == SERVICE_EXIT_SCRIPT && svc->exitscript[0]) {
            serial_write_str("[SERVICES] '");
            serial_write_str(service_display_name(svc));
            serial_write_str("' running exit script\n");
            command_execute(svc->exitscript);
        }

        return;
    }
}

/* ===========================================
 * Discovery
 * =========================================== */

void service_manager_init(void) {
    if (!minimafs_exists(SERVICES_DIR) || !minimafs_is_dir(SERVICES_DIR)) {
        serial_write_str("[SERVICES] No " SERVICES_DIR " directory, skipping\n");
        return;
    }

    minimafs_dir_entry_t* entries = (minimafs_dir_entry_t*)alloc(
        MINIMAFS_MAX_ROOT_ENTRIES * sizeof(minimafs_dir_entry_t));
    if (!entries) {
        serial_write_str("[SERVICES] OOM listing " SERVICES_DIR "\n");
        return;
    }

    uint32_t count = minimafs_list_dir(SERVICES_DIR, entries, MINIMAFS_MAX_ROOT_ENTRIES);
    serial_write_str("[SERVICES] Scanning ");
    serial_write_dec(count);
    serial_write_str(" entries in " SERVICES_DIR "\n");

    for (uint32_t i = 0; i < count && g_service_count < SERVICE_MAX_COUNT; i++) {
        if (entries[i].type == MINIMAFS_TYPE_DIR) continue;
        if (!service_has_extension(entries[i].name, "service")) continue;

        char path[MINIMAFS_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", SERVICES_DIR, entries[i].name);

        service_entry_t* svc = &g_services[g_service_count];
        if (!service_parse_file(path, svc)) {
            serial_write_str("[SERVICES] Failed to parse ");
            serial_write_str(path);
            serial_write_str("\n");
            continue;
        }

        strncpy(svc->filename, entries[i].name, sizeof(svc->filename) - 1);
        svc->filename[sizeof(svc->filename) - 1] = '\0';

        if (!svc->has_exec && !svc->has_script) {
            serial_write_str("[SERVICES] '");
            serial_write_str(path);
            serial_write_str("' has no [STARTUP] exec or script, skipping\n");
            continue;
        }

        svc->active = true;
        g_service_count++;

        graphics_write_textr("[STARTING] ");
        graphics_write_textr(service_display_name(svc));
        graphics_write_textr("\n");

        char run_arg[MINIMAFS_MAX_PATH];
        bool direct_run = svc->has_exec &&
                          service_script_run_path(svc->exec_path, run_arg,
                                                  sizeof(run_arg));
        if (svc->has_exec && !direct_run && svc->exec_path[0] != '/' &&
            svc->exec_path[0] != '.' && !strchr(svc->exec_path, ':')) {
            command_execute(svc->exec_path);
            svc->startup_ran = true;
        }

        process_t* proc = createProcess(svc->filename, service_monitor_entry);
        if (!proc) {
            serial_write_str("[SERVICES] Failed to create monitor process for ");
            serial_write_str(svc->filename);
            serial_write_str("\n");
            svc->active = false;
            g_service_count--;
            continue;
        }

        serial_write_str("[SERVICES] Registered service: ");
        serial_write_str(service_display_name(svc));
        serial_write_str("\n");
    }

    free_mem(entries);
}

/* ===========================================
 * 'services' debug command
 * =========================================== */

static void cmd_services(int argc, const char** argv) {
    if (g_service_count == 0) {
        graphics_write_textr("No services registered\n");
        return;
    }

    graphics_write_textr("Registered services:\n");
    for (int i = 0; i < g_service_count; i++) {
        service_entry_t* svc = &g_services[i];
        if (!svc->active) continue;

        graphics_write_textr("  ");
        graphics_write_textr(service_display_name(svc));
        graphics_write_textr(" (");
        graphics_write_textr(svc->filename);
        graphics_write_textr(")\n");

        if (svc->desc[0]) {
            graphics_write_textr("    ");
            graphics_write_textr(svc->desc);
            graphics_write_textr("\n");
        }
    }
}

void register_services(void) {
    command_register("services", cmd_services);
}

REGISTER_COMMAND(register_services);
