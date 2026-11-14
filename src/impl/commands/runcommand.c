#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "string.h"
#include "serial.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "x86_64/loader/elfloader.h"
#include "x86_64/runfile.h"
#include "x86_64/minimafs.h"
#include "x86_64/runcommand.h"
#include "prochandler.h"
#include "x86_64/scheduler.h"
#include "x86_64/loader/argblock.h"
#include "x86_64/uvm.h"
#include "x86_64/spawn.h"

// The main stack: 8 MiB unless the launcher asks for another size. Its
// pages cost memory only once they are used.
#define RUN_STACK_SIZE   (8ull * 1024 * 1024)
#define RUN_STACK_MIN    (64ull * 1024)
#define RUN_STACK_MAX    (1024ull * 1024 * 1024)
#define RUN_STACK_GUARD  (64ull * 1024)

// The argument block (argv/envp, see argblock.h) has a mapping of its own
// above the stack: at least this much, and at most RUN_ARGV_MAX_RESERVED.
#define RUN_ARGV_MIN_RESERVED (4 * 1024)
#define RUN_ARGV_MAX_RESERVED (64 * 1024)

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

// Where main.elf lies in an open program file: a .run bundle keeps it as
// one of its entries, a plain ELF executable is the whole file.
typedef struct {
    minimafs_file_handle_t* file;
    uint32_t offset;
    uint32_t size;
} run_elf_source_t;

static uint32_t run_elf_read(void* ctx, uint64_t offset, void* buffer, uint32_t size) {
    run_elf_source_t* src = (run_elf_source_t*)ctx;
    if (offset > src->size || size > src->size - offset) return 0;
    if (!minimafs_seek(src->file, src->offset + (uint32_t)offset)) return 0;
    return minimafs_read(src->file, buffer, size);
}

// Opens the program and finds its ELF. The caller closes src->file.
static bool run_open_main_elf(const char* path, run_elf_source_t* src) {
    memset(src, 0, sizeof(*src));
    minimafs_file_handle_t* file = NULL;
    bool exists = minimafs_exists(path);
    bool is_dir = exists && minimafs_is_dir(path);
    serial_write_str("run: opening bundle path=");
    serial_write_str(path ? path : "<null>");
    serial_write_str(" exists=");
    serial_write_dec(exists ? 1 : 0);
    serial_write_str(" dir=");
    serial_write_dec(is_dir ? 1 : 0);
    serial_write_str("\n");
    for (int attempt = 0; attempt < 20 && !file; attempt++) {
        file = minimafs_open(path, true);
        if (!file && (attempt == 0 || attempt == 19)) {
            serial_write_str("run: bundle open attempt ");
            serial_write_dec((uint64_t)(attempt + 1));
            serial_write_str(" failed\n");
        }
        if (!file) sleep(10);
    }
    if (!file) {
        serial_write_str("run: could not open bundle path=");
        serial_write_str(path ? path : "<null>");
        serial_write_str("\n");
        return false;
    }

    uint32_t archive_size = minimafs_size(file);
    serial_write_str("run: bundle opened size=");
    serial_write_dec(archive_size);
    serial_write_str("\n");
    uint8_t header[MINIMALOS_RUN_HEADER_SIZE];
    if (archive_size < sizeof(header) ||
        minimafs_read(file, header, sizeof(header)) != sizeof(header)) {
        serial_write_str("run: bundle is empty or too short\n");
        minimafs_close(file);
        return false;
    }
    if (header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F') {
        // A plain ELF executable rather than a .run bundle (SYS_SPAWN runs
        // both, e.g. compiler passes installed as bare binaries).
        src->file = file;
        src->offset = 0;
        src->size = archive_size;
        return true;
    }
    if (memcmp(header, MINIMALOS_RUN_MAGIC, MINIMALOS_RUN_MAGIC_SIZE) != 0 ||
        run_read_u32(header + 8) != MINIMALOS_RUN_VERSION) {
        serial_write_str("run: bundle header is invalid\n");
        minimafs_close(file);
        return false;
    }

    uint32_t count = run_read_u32(header + 12);
    serial_write_str("run: bundle entries=");
    serial_write_dec(count);
    serial_write_str("\n");
    if (count == 0 || count > (archive_size - MINIMALOS_RUN_HEADER_SIZE) /
        MINIMALOS_RUN_ENTRY_SIZE) {
        minimafs_close(file);
        return false;
    }
    uint32_t table_bytes = count * MINIMALOS_RUN_ENTRY_SIZE;
    uint32_t table_size = MINIMALOS_RUN_HEADER_SIZE + table_bytes;
    uint8_t* table = (uint8_t*)alloc_unzeroed(table_bytes);
    if (!table || !minimafs_seek(file, MINIMALOS_RUN_HEADER_SIZE) ||
        minimafs_read(file, table, table_bytes) != table_bytes) {
        serial_write_str("run: cannot read the bundle table\n");
        if (table) free_mem(table);
        minimafs_close(file);
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < count && !found; i++) {
        const uint8_t* entry = table + i * MINIMALOS_RUN_ENTRY_SIZE;
        uint32_t offset = run_read_u32(entry + MINIMALOS_RUN_NAME_SIZE);
        uint32_t size = run_read_u32(entry + MINIMALOS_RUN_NAME_SIZE + 4);
        if (strncmp((const char*)entry, "main.elf", 8) != 0 || entry[8] != '\0') continue;
        if (offset < table_size || offset > archive_size || size > archive_size - offset) {
            serial_write_str("run: bundle entry invalid offset=");
            serial_write_dec(offset);
            serial_write_str(" size=");
            serial_write_dec(size);
            serial_write_str("\n");
            continue;
        }
        src->file = file;
        src->offset = offset;
        src->size = size;
        found = true;
    }
    free_mem(table);
    if (!found) {
        serial_write_str("run: no main.elf in the bundle\n");
        minimafs_close(file);
    }
    return found;
}

// Bytes mapped for the argument block: whole pages.
static size_t run_reserved_for(size_t block_bytes) {
    size_t reserved = (block_bytes + 0xFFFu) & ~(size_t)0xFFFu;
    return reserved < RUN_ARGV_MIN_RESERVED ? RUN_ARGV_MIN_RESERVED : reserved;
}

static void run_drop_sinks(const run_launch_t* launch) {
    if (launch->out_sink) stdio_sink_put(launch->out_sink);
    if (launch->err_sink) stdio_sink_put(launch->err_sink);
}

static char* run_dup_bytes(const char* data, size_t len) {
    if (!data || !len) return NULL;
    char* copy = (char*)alloc_unzeroed(len + 1);
    if (!copy) return NULL;
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

process_t* run_launch_ex(const run_launch_t* launch) {
    if (!launch || !launch->path) return NULL;
    uint64_t started_pid = 0;
    const char* run_path = launch->path;
    const char* default_argv[1] = { run_path };
    uint32_t argc = launch->argc ? launch->argc : 1;
    const char* const* argv = launch->argc ? launch->argv : default_argv;

    size_t block_bytes = argblock_size(argc, argv, launch->env, launch->env_len);
    if (block_bytes == 0 || block_bytes > RUN_ARGV_MAX_RESERVED) {
        serial_write_str("run: too many/long arguments or environment, not launching\n");
        run_drop_sinks(launch);
        return NULL;
    }
    size_t reserved = run_reserved_for(block_bytes);
    uint64_t stack_size = launch->stack_size ? launch->stack_size : RUN_STACK_SIZE;
    if (stack_size < RUN_STACK_MIN) stack_size = RUN_STACK_MIN;
    if (stack_size > RUN_STACK_MAX) stack_size = RUN_STACK_MAX;
    stack_size = (stack_size + 0xFFFull) & ~0xFFFull;

    run_elf_source_t source;
    if (!run_open_main_elf(run_path, &source)) {
        serial_write_str("run: failed to find main.elf\n");
        run_drop_sinks(launch);
        return NULL;
    }
    elf_stream_t* elf = (elf_stream_t*)alloc_unzeroed(sizeof(*elf));
    // The argument block is built here and copied into the program.
    void* block = alloc(reserved);
    char** user_argv = NULL;
    char** user_envp = NULL;
    if (!elf || !block ||
        !elf_stream_open(elf, run_elf_read, &source, source.size) ||
        !argblock_build(block, reserved, UVM_ARGS_BASE, argc, argv,
                        launch->env, launch->env_len, &user_argv, &user_envp)) {
        serial_write_str("run: failed to read main.elf or build the arguments\n");
        if (elf) free_mem(elf);
        if (block) free_mem(block);
        minimafs_close(source.file);
        run_drop_sinks(launch);
        return NULL;
    }

    char procname[96];
    snprintf(procname, sizeof(procname), "%s", run_path);

    // A .run program is user-loaded code, not a kernel-owned
    // housekeeping process - classify it as such. See
    // process_privilege_t in proc.h for exactly what this does (and
    // does not yet) enforce.
    uint64_t entry_point = UVM_IMAGE_BASE + elf->header.e_entry;
    process_t* proc = createUserProcess(procname, (void (*)())(uintptr_t)entry_point);
    if (proc) {
        // The process trampoline switches to ring3 using the application's
        // stack.
        // SysV AMD64 requires RSP % 16 == 8 at a function entry point.
        // proc_enter_ring3 uses iretq directly, so reserve the return-slot
        // word that a normal call would have placed on the stack.
        proc->user_stack = (uint64_t*)(uintptr_t)(UVM_STACK_TOP - sizeof(uint64_t));
        proc->user_argc  = argc;
        proc->user_argv  = user_argv;
        proc->user_envp  = user_envp;
        if (launch->parent_pid) proc->parent_pid = launch->parent_pid;
        proc->wait_record = launch->wait_record;
        // Reaping frees all of these, also when the launch fails below.
        proc->out_sink = launch->out_sink;
        proc->err_sink = launch->err_sink;
        proc->env = run_dup_bytes(launch->env, launch->env_len);
        proc->env_len = proc->env ? launch->env_len : 0;
        proc->cwd = launch->cwd ? run_dup_bytes(launch->cwd, strlen(launch->cwd)) : NULL;

        // The address space: image, main stack with a guard below it that
        // nothing else can be mapped into, and the argument block. Stack
        // pages get their memory when they are first used.
        uint64_t stack_base = UVM_STACK_TOP - stack_size;
        bool ok = proc->uvm &&
            elf_stream_load(elf, proc, UVM_IMAGE_BASE, &entry_point) &&
            uvm_map(proc, stack_base, stack_size, UVM_PROT_READ | UVM_PROT_WRITE,
                    UVM_MAP_FIXED) == stack_base &&
            uvm_map(proc, stack_base - RUN_STACK_GUARD, RUN_STACK_GUARD, UVM_PROT_NONE,
                    UVM_MAP_FIXED) == stack_base - RUN_STACK_GUARD &&
            uvm_map(proc, UVM_ARGS_BASE, reserved, UVM_PROT_READ | UVM_PROT_WRITE,
                    UVM_MAP_FIXED) == UVM_ARGS_BASE &&
            uvm_copy_in(proc, UVM_ARGS_BASE, block, reserved);
        if (!ok) {
            serial_write_str("run: failed to set up the program's memory\n");
            // It never ran and its pid was never handed out: no exit
            // record, or SYS_WAIT(any) would report a child nobody started.
            proc->wait_record = false;
            kill(proc);
            proc = NULL;
        } else {
            proc->user_image_address = UVM_IMAGE_BASE;
            proc->user_image_size = elf->image_size;
            proc->user_stack_address = stack_base;
            proc->user_stack_size = stack_size;
            started_pid = proc->pid;
            if (launch->out_pid) *launch->out_pid = started_pid;
            proc_start(proc);       // from here it may run and be freed on another CPU
        }
    } else {
        run_drop_sinks(launch);
        serial_write_str("run: failed to create the user process\n");
    }
    free_mem(elf);
    free_mem(block);
    minimafs_close(source.file);
    if (!proc) return NULL;

    if (launch->terminal_foreground) command_set_last_user_pid(started_pid);

    return proc;
}

process_t* run_launch_file(const char* run_path, int extra_argc, const char** extra_argv) {
    return run_launch_file_pid(run_path, extra_argc, extra_argv, NULL);
}

process_t* run_launch_file_pid(const char* run_path, int extra_argc, const char** extra_argv,
                               uint64_t* out_pid) {
    if (!run_path) return NULL;
    if (extra_argc < 0) extra_argc = 0;
    if ((uint32_t)extra_argc + 1u > ARGBLOCK_MAX_ARGS) return NULL;

    // argv[0] is always the path; the extra arguments follow it.
    const char** argv = (const char**)alloc_unzeroed(((size_t)extra_argc + 1u) * sizeof(char*));
    if (!argv) return NULL;
    argv[0] = run_path;
    for (int i = 0; i < extra_argc; i++)
        argv[1 + i] = (extra_argv && extra_argv[i]) ? extra_argv[i] : "";

    run_launch_t launch;
    memset(&launch, 0, sizeof(launch));
    launch.path = run_path;
    launch.argc = (uint32_t)extra_argc + 1u;
    launch.argv = argv;
    launch.terminal_foreground = true;
    launch.out_pid = out_pid;
    process_t* proc = run_launch_ex(&launch);
    free_mem(argv);
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

    // Started quietly: the program's own output is what matters.
    serial_write_str("run: started ");
    serial_write_str(run_path);
    serial_write_str("\n");
}

void register_run(void) {
    command_register("run", cmd_run);
}

REGISTER_COMMAND(register_run);
