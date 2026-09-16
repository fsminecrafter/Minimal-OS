#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "string.h"
#include "serial.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/allocator.h"
#include "x86_64/loader/elfloader.h"
#include "x86_64/runfile.h"
#include "x86_64/minimafs.h"
#include "x86_64/runcommand.h"
#include "prochandler.h"
#include "x86_64/scheduler.h"

#define RUN_STACK_SIZE   (64 * 1024)

// Bytes carved out of the low end of a launched process's stack
// allocation to hold its argv pointer array plus the argument string
// data itself (see run_build_argv_block()). The stack pointer starts
// at the TOP of the allocation and grows down, so this only collides
// with real stack usage if the program's own call stack ever grows to
// within this many bytes of the bottom - the same fixed-size, no-
// guard-page tradeoff every .run process's stack already accepts.
#define RUN_ARGV_RESERVED_SIZE (4 * 1024)

// Hard cap on argc (argv[0] plus every extra argument) a launched
// process can receive. Large enough for any reasonable command line;
// keeps run_build_argv_block()'s on-stack scratch array a fixed, small
// size regardless of what a caller passes in.
#define RUN_MAX_ARGS 64

static uint32_t run_read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char* run_normalize_path(const char* input, char* normalized,
                               size_t normalized_size) {
    if (!input || !normalized || normalized_size == 0) return NULL;

    while (input[0] == '.' && input[1] == '/') input += 2;
    if (strchr(input, ':')) {
        if (strlen(input) >= normalized_size) return NULL;
        strcpy(normalized, input);
    } else {
        if (snprintf(normalized, normalized_size, "0:/%s", input) >=
            (int)normalized_size) return NULL;
    }
    return normalized;
}

static bool run_extract_main_elf(const char* path, uint8_t** elf_data,
                                 uint32_t* elf_size) {
    minimafs_file_handle_t* file = minimafs_open(path, true);
    if (!file) return false;

    uint32_t archive_size = minimafs_size(file);
    uint8_t* archive = (uint8_t*)alloc_unzeroed(archive_size);
    if (!archive) { minimafs_close(file); return false; }

    bool ok = minimafs_read(file, archive, archive_size) == archive_size;
    minimafs_close(file);
    if (!ok || archive_size < MINIMALOS_RUN_HEADER_SIZE ||
        memcmp(archive, MINIMALOS_RUN_MAGIC, MINIMALOS_RUN_MAGIC_SIZE) != 0 ||
        run_read_u32(archive + 8) != MINIMALOS_RUN_VERSION) {
        free_mem(archive);
        return false;
    }

    uint32_t count = run_read_u32(archive + 12);
    if (count == 0 || count > (archive_size - MINIMALOS_RUN_HEADER_SIZE) /
        MINIMALOS_RUN_ENTRY_SIZE) {
        free_mem(archive);
        return false;
    }
    uint32_t table_size = MINIMALOS_RUN_HEADER_SIZE + count * MINIMALOS_RUN_ENTRY_SIZE;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = archive + MINIMALOS_RUN_HEADER_SIZE + i * MINIMALOS_RUN_ENTRY_SIZE;
        uint32_t offset = run_read_u32(entry + MINIMALOS_RUN_NAME_SIZE);
        uint32_t size = run_read_u32(entry + MINIMALOS_RUN_NAME_SIZE + 4);
        if (strncmp((const char*)entry, "main.elf", 8) != 0 || entry[8] != '\0' ||
            offset < table_size || offset > archive_size ||
            size > archive_size - offset) continue;

        uint8_t* extracted = (uint8_t*)alloc_unzeroed(size);
        if (!extracted) break;
        memcpy(extracted, archive + offset, size);
        free_mem(archive);
        *elf_data = extracted;
        *elf_size = size;
        return true;
    }

    free_mem(archive);
    return false;
}

// Lays out an argv pointer array followed by NUL-terminated string
// data inside the first RUN_ARGV_RESERVED_SIZE bytes of
// `stack_base`/`stack_size` (the process's not-yet-mapped kernel stack
// buffer). argv[0] is always `run_path`; `extra_argc`/`extra_argv`
// supply everything after that (e.g. "-h", "0:/file" typed after the
// program name on the command line).
//
// The array and strings are written into ordinary kernel heap memory
// at this point - proc_map_user_range() (called by the caller right
// after this) copies that memory verbatim into the new process's own
// physical pages and maps them at this same virtual address, so the
// pointer values stored in `*out_argv` remain valid once the process's
// private PML4 is active. See user_stack in proc.h for the identical
// trick already used for the stack pointer itself.
//
// Returns false (leaving *out_argc/*out_argv untouched) if there are
// too many arguments (see RUN_MAX_ARGS) or the reserved region is too
// small to hold them all - callers should treat that as a launch
// failure rather than silently truncating a program's argv.
static bool run_build_argv_block(void* stack_base, size_t stack_size,
                                 const char* run_path,
                                 int extra_argc, const char** extra_argv,
                                 uint64_t* out_argc, char*** out_argv) {
    if (!stack_base || !run_path || !out_argc || !out_argv) return false;
    if (stack_size < RUN_ARGV_RESERVED_SIZE) return false;
    if (extra_argc < 0) extra_argc = 0;

    uint32_t total_argc = 1u + (uint32_t)extra_argc;
    if (total_argc == 0 || total_argc > RUN_MAX_ARGS) return false;

    const char* sources[RUN_MAX_ARGS];
    sources[0] = run_path;
    for (uint32_t i = 0; i < (uint32_t)extra_argc; i++) {
        sources[1 + i] = (extra_argv && extra_argv[i]) ? extra_argv[i] : "";
    }

    uint8_t* region = (uint8_t*)stack_base;
    size_t region_size = RUN_ARGV_RESERVED_SIZE;

    // Pointer array first (NULL-terminated, matching the conventional
    // argv[argc] == NULL every C runtime expects), string bytes right
    // after it.
    char** argv_array = (char**)region;
    size_t array_bytes = (size_t)(total_argc + 1) * sizeof(char*);
    if (array_bytes >= region_size) return false;

    uint8_t* str_cursor = region + array_bytes;
    size_t str_remaining = region_size - array_bytes;

    for (uint32_t i = 0; i < total_argc; i++) {
        size_t len = strlen(sources[i]) + 1;
        if (len > str_remaining) return false;
        memcpy(str_cursor, sources[i], len);
        argv_array[i] = (char*)str_cursor;
        str_cursor += len;
        str_remaining -= len;
    }
    argv_array[total_argc] = NULL;

    *out_argc = total_argc;
    *out_argv = argv_array;
    return true;
}

process_t* run_launch_file(const char* run_path, int extra_argc, const char** extra_argv) {
    elf_loaded_image_t image;
    uint8_t* elf_data = NULL;
    uint32_t elf_size = 0;
    if (!run_extract_main_elf(run_path, &elf_data, &elf_size) ||
        !elf_load_buffer(elf_data, elf_size, &image)) {
        return NULL;
    }

    void* stack = alloc_unzeroed(RUN_STACK_SIZE);
    if (!stack) {
        elf_unload(&image);
        return NULL;
    }

    // alloc_unzeroed() does not zero memory; run_build_argv_block()
    // only writes exactly as many bytes as each string needs, so
    // clear the reserved region first to guarantee no stale heap
    // bytes ever leak into an unused array slot or padding.
    memset(stack, 0, RUN_ARGV_RESERVED_SIZE);

    uint64_t user_argc = 0;
    char** user_argv = NULL;
    if (!run_build_argv_block(stack, RUN_STACK_SIZE, run_path,
                              extra_argc, extra_argv, &user_argc, &user_argv)) {
        serial_write_str("run: too many/long arguments, not launching\n");
        free_mem(stack);
        elf_unload(&image);
        return NULL;
    }

    char procname[96];
    snprintf(procname, sizeof(procname), "%s", run_path);

    // A .run program is user-loaded code, not a kernel-owned
    // housekeeping process - classify it as such. See
    // process_privilege_t in proc.h for exactly what this does (and
    // does not yet) enforce.
    process_t* proc = createUserProcess(procname, (void (*)())image.entry_point);
    if (proc) {
        // The process trampoline switches to ring3 using the application's
        // stack. Keep the image and stack alive until the process exits.
        proc->user_stack = (uint64_t*)((uint8_t*)stack + RUN_STACK_SIZE);
        proc->user_argc  = user_argc;
        proc->user_argv  = user_argv;
        if (!proc_map_user_range(proc, image.base, image.image_size) ||
            !proc_map_user_range(proc, stack, RUN_STACK_SIZE)) {
            kill(proc);
            proc = NULL;
        } else {
            // The process now owns PMM-backed copies of both ranges.
            elf_unload(&image);
            free_mem(stack);
        }
    }
    if (!proc) {
        free_mem(stack);
        elf_unload(&image);
        return NULL;
    }

    command_set_last_user_pid(proc->pid);

    return proc;
}

void cmd_run(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: run <path-to.run-file> [args...]\n");
        return;
    }

    char path[MINIMAFS_MAX_PATH];
    const char* run_path = run_normalize_path(argv[1], path, sizeof(path));
    if (!run_path) {
        graphics_write_textr("run: invalid path\n");
        return;
    }

    // Everything after the path becomes the launched program's own
    // argv[1..] (argv[0] is always the path itself - see
    // run_build_argv_block()). This is what lets both
    // "run 0:/hello.run -h foo" and, via the file-association
    // dispatcher in commandhandler.c, "./hello.run -h foo" typed
    // directly at the prompt forward options through to the program.
    int extra_argc = argc - 2;
    const char** extra_argv = (extra_argc > 0) ? &argv[2] : NULL;

    process_t* proc = run_launch_file(run_path, extra_argc, extra_argv);
    if (!proc) {
        graphics_write_textr("run: failed to load ");
        graphics_write_textr(run_path);
        graphics_write_textr("\n");
        return;
    }

    graphics_write_textr("Started: ");
    graphics_write_textr(run_path);
    graphics_write_textr(" (PID ");
    graphics_write_textr_udec(proc->pid);
    graphics_write_textr(")\n");
}

void register_run(void) {
    command_register("run", cmd_run);
}

REGISTER_COMMAND(register_run);
