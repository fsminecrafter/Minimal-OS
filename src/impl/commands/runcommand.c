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
#include "prochandler.h"
#include "x86_64/scheduler.h"

#define RUN_STACK_SIZE   (64 * 1024)

static uint32_t run_read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char* run_normalize_path(const char* input, char* normalized,
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

void cmd_run(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: run <path-to.run-file>\n");
        return;
    }

    char path[MINIMAFS_MAX_PATH];
    const char* run_path = run_normalize_path(argv[1], path, sizeof(path));
    if (!run_path) {
        graphics_write_textr("run: invalid path\n");
        return;
    }

    elf_loaded_image_t image;
    uint8_t* elf_data = NULL;
    uint32_t elf_size = 0;
    if (!run_extract_main_elf(run_path, &elf_data, &elf_size) ||
        !elf_load_buffer(elf_data, elf_size, &image)) {
        graphics_write_textr("run: failed to load ");
        graphics_write_textr(run_path);
        graphics_write_textr("\n");
        return;
    }

    void* stack = alloc_unzeroed(RUN_STACK_SIZE);
    if (!stack) {
        graphics_write_textr("run: out of memory allocating stack\n");
        elf_unload(&image);
        return;
    }

    char procname[96];
    snprintf(procname, sizeof(procname), "%s", run_path);

    // A .run program is user-loaded code, not a kernel-owned
    // housekeeping process - classify it as such. See
    // process_privilege_t in proc.h for exactly what this does (and
    // does not yet) enforce: today it's metadata only, since there is
    // no per-process address space or user GDT segment to actually
    // isolate it at CPL3 with.
    process_t* proc = createUserProcess(procname, (void (*)())image.entry_point);
    if (proc) {
        // The process trampoline switches to ring3 using the application's
        // stack. Keep the image and stack alive until the process exits.
        proc->user_stack = (uint64_t*)((uint8_t*)stack + RUN_STACK_SIZE);
        if (!proc_map_user_range(proc, image.base, image.image_size) ||
            !proc_map_user_range(proc, stack, RUN_STACK_SIZE)) {
            graphics_write_textr("run: failed to map user image\n");
            kill(proc);
            proc = NULL;
        } else {
            // The process now owns PMM-backed copies of both ranges.
            elf_unload(&image);
            free_mem(stack);
        }
    }
    if (!proc) {
        graphics_write_textr("run: failed to create process\n");
        free_mem(stack);
        elf_unload(&image);
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