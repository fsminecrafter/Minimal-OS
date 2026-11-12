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
#include "x86_64/spawn.h"

#define RUN_STACK_SIZE   (64 * 1024)
#define RUN_USER_IMAGE_BASE 0x20000000ULL
#define RUN_USER_STACK_BASE 0x30000000ULL

// The argument block (argv/envp, see argblock.h) takes at least this much
// of the low end of the stack allocation, and at most RUN_ARGV_MAX_RESERVED.
#define RUN_ARGV_MIN_RESERVED (4 * 1024)
#define RUN_ARGV_MAX_RESERVED (64 * 1024)

static void run_release_buffer(void* buffer, bool page_backed, size_t page_count) {
    if (!buffer) return;
    if (page_backed)
        free_pages(buffer, page_count);
    else
        free_mem(buffer);
}

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
        serial_write_str(" exists=");
        serial_write_dec(minimafs_exists(path) ? 1 : 0);
        serial_write_str(" dir=");
        serial_write_dec(minimafs_is_dir(path) ? 1 : 0);
        serial_write_str("\n");
        return false;
    }

    uint32_t archive_size = minimafs_size(file);
    serial_write_str("run: bundle opened size=");
    serial_write_dec(archive_size);
    serial_write_str("\n");
    if (archive_size == 0) {
        serial_write_str("run: bundle is empty\n");
        minimafs_close(file);
        return false;
    }
    uint8_t header[MINIMALOS_RUN_HEADER_SIZE];
    uint32_t header_read = minimafs_read(file, header, sizeof(header));
    if (header_read != sizeof(header)) {
        serial_write_str("run: bundle header read short bytes=");
        serial_write_dec(header_read);
        serial_write_str("\n");
        minimafs_close(file);
        return false;
    }
    if (header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F') {
        // A plain ELF executable rather than a .run bundle (SYS_SPAWN runs
        // both, e.g. compiler passes installed as bare binaries).
        uint8_t* image = (uint8_t*)alloc_unzeroed(archive_size);
        size_t image_pages = 0;
        if (!image) {
            image_pages = (archive_size + 0xFFFu) / 0x1000u;
            image = (uint8_t*)alloc_pages_zeroed(image_pages);
            if (!image || !elf_register_page_buffer(image, image_pages)) {
                if (image) free_pages(image, image_pages);
                serial_write_str("run: ELF allocation failed bytes=");
                serial_write_dec(archive_size);
                serial_write_str("\n");
                minimafs_close(file);
                return false;
            }
        }
        memcpy(image, header, sizeof(header));
        uint32_t rest = archive_size - (uint32_t)sizeof(header);
        if (rest && minimafs_read(file, image + sizeof(header), rest) != rest) {
            serial_write_str("run: ELF read short\n");
            if (image_pages) free_pages(image, image_pages);
            else free_mem(image);
            minimafs_close(file);
            return false;
        }
        minimafs_close(file);
        *elf_data = image;
        *elf_size = archive_size;
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
    uint32_t table_size = MINIMALOS_RUN_HEADER_SIZE + count * MINIMALOS_RUN_ENTRY_SIZE;
    uint8_t* table = (uint8_t*)alloc_unzeroed(count * MINIMALOS_RUN_ENTRY_SIZE);
    if (!table) {
        serial_write_str("run: bundle table allocation failed bytes=");
        serial_write_dec(count * MINIMALOS_RUN_ENTRY_SIZE);
        serial_write_str("\n");
        minimafs_close(file);
        return false;
    }
    if (!minimafs_seek(file, MINIMALOS_RUN_HEADER_SIZE)) {
        serial_write_str("run: bundle table seek failed\n");
        free_mem(table);
        minimafs_close(file);
        return false;
    }
    uint32_t table_read = minimafs_read(file, table, count * MINIMALOS_RUN_ENTRY_SIZE);
    if (table_read != count * MINIMALOS_RUN_ENTRY_SIZE) {
        serial_write_str("run: bundle table read short bytes=");
        serial_write_dec(table_read);
        serial_write_str(" expected=");
        serial_write_dec(count * MINIMALOS_RUN_ENTRY_SIZE);
        serial_write_str("\n");
        if (table) free_mem(table);
        minimafs_close(file);
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = table + i * MINIMALOS_RUN_ENTRY_SIZE;
        uint32_t offset = run_read_u32(entry + MINIMALOS_RUN_NAME_SIZE);
        uint32_t size = run_read_u32(entry + MINIMALOS_RUN_NAME_SIZE + 4);
        if (strncmp((const char*)entry, "main.elf", 8) != 0 || entry[8] != '\0' ||
            offset < table_size || offset > archive_size ||
            size > archive_size - offset) {
            serial_write_str("run: bundle entry invalid offset=");
            serial_write_dec(offset);
            serial_write_str(" size=");
            serial_write_dec(size);
            serial_write_str("\n");
            continue;
        }

        free_mem(table);
        table = NULL;
        minimafs_close(file);
        file = NULL;
        uint8_t* extracted = (uint8_t*)alloc_unzeroed(size);
        size_t extracted_pages = 0;
        if (!extracted) {
            extracted_pages = (size + 0xFFFu) / 0x1000u;
            extracted = (uint8_t*)alloc_pages_zeroed(extracted_pages);
            if (!extracted || !elf_register_page_buffer(extracted, extracted_pages)) {
                if (extracted) free_pages(extracted, extracted_pages);
                serial_write_str("run: main.elf allocation failed bytes=");
                serial_write_dec(size);
                serial_write_str(" free=");
                serial_write_dec(allocator_free_bytes());
                serial_write_str(" largest=");
                serial_write_dec(allocator_largest_free_block());
                serial_write_str("\n");
                break;
            }
        }
        file = minimafs_open(path, true);
        if (!file) {
            serial_write_str("run: bundle reopen failed\n");
            if (extracted_pages) free_pages(extracted, extracted_pages);
            else free_mem(extracted);
            break;
        }
        if (!minimafs_seek(file, offset)) {
            serial_write_str("run: main.elf seek failed offset=");
            serial_write_dec(offset);
            serial_write_str("\n");
            if (extracted_pages) free_pages(extracted, extracted_pages);
            else free_mem(extracted);
            minimafs_close(file);
            break;
        }
        uint32_t payload_read = minimafs_read(file, extracted, size);
        if (payload_read != size) {
            serial_write_str("run: main.elf read short bytes=");
            serial_write_dec(payload_read);
            serial_write_str(" expected=");
            serial_write_dec(size);
            serial_write_str("\n");
            if (extracted_pages) free_pages(extracted, extracted_pages);
            else free_mem(extracted);
            minimafs_close(file);
            break;
        }
        minimafs_close(file);
        *elf_data = extracted;
        *elf_size = size;
        return true;
    }

    free_mem(table);
    minimafs_close(file);
    return false;
}

// The argument block (argv, envp and their strings; see argblock.h) sits
// at the low end of the stack allocation. At least RUN_ARGV_MIN_RESERVED
// bytes are set aside, and the stack grows by however much more a long
// command line or a big environment needs, so the program keeps the same
// amount of stack either way.
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
    size_t stack_size = RUN_STACK_SIZE - RUN_ARGV_MIN_RESERVED + reserved;

    elf_loaded_image_t image;
    uint8_t* elf_data = NULL;
    uint32_t elf_size = 0;
    if (!run_extract_main_elf(run_path, &elf_data, &elf_size)) {
        serial_write_str("run: failed to extract main.elf\n");
        run_drop_sinks(launch);
        return NULL;
    }
    if (!elf_load_buffer_at(elf_data, elf_size, RUN_USER_IMAGE_BASE, &image)) {
        serial_write_str("run: failed to load main.elf\n");
        run_drop_sinks(launch);
        return NULL;
    }

    void* stack = alloc_unzeroed(stack_size);
    bool stack_page_backed = false;
    size_t stack_page_count = 0;
    if (!stack) {
        stack_page_count = (stack_size + 0xFFFu) / 0x1000u;
        stack = alloc_pages_zeroed(stack_page_count);
        if (!stack) {
            serial_write_str("run: failed to allocate process stack\n");
            elf_unload(&image);
            run_drop_sinks(launch);
            return NULL;
        }
        stack_page_backed = true;
    }

    // alloc_unzeroed() does not zero memory: clear the reserved region so
    // no stale heap bytes leak into padding the program can see.
    memset(stack, 0, reserved);

    char** user_argv = NULL;
    char** user_envp = NULL;
    if (!argblock_build(stack, reserved, RUN_USER_STACK_BASE, argc, argv,
                        launch->env, launch->env_len, &user_argv, &user_envp)) {
        serial_write_str("run: failed to build process argv\n");
        run_release_buffer(stack, stack_page_backed, stack_page_count);
        elf_unload(&image);
        run_drop_sinks(launch);
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
        // SysV AMD64 requires RSP % 16 == 8 at a function entry point.
        // proc_enter_ring3 uses iretq directly, so reserve the return-slot
        // word that a normal call would have placed on the stack.
        proc->user_stack = (uint64_t*)(RUN_USER_STACK_BASE + stack_size - sizeof(uint64_t));
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
        if (!proc_map_user_range_at(proc, image.base,
                                    (void*)RUN_USER_IMAGE_BASE, image.image_size) ||
            !proc_map_user_range_at(proc, stack,
                                    (void*)RUN_USER_STACK_BASE, stack_size)) {
            serial_write_str("run: failed to map process image or stack\n");
            // It never ran and its pid was never handed out: no exit
            // record, or SYS_WAIT(any) would report a child nobody started.
            proc->wait_record = false;
            kill(proc);
            proc = NULL;
        } else {
            // The process now owns PMM-backed copies of both ranges.
            elf_unload(&image);
            run_release_buffer(stack, stack_page_backed, stack_page_count);
            proc_start(proc);
        }
    } else {
        run_drop_sinks(launch);
    }
    if (!proc) {
        serial_write_str("run: failed to create user process after loading image\n");
        run_release_buffer(stack, stack_page_backed, stack_page_count);
        elf_unload(&image);
        return NULL;
    }

    if (launch->terminal_foreground) command_set_last_user_pid(proc->pid);

    return proc;
}

process_t* run_launch_file(const char* run_path, int extra_argc, const char** extra_argv) {
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
