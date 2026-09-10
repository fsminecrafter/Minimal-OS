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
#include "x86_64/spinlock.h"
#include "x86_64/minimafs.h"
#include "prochandler.h"
#include "x86_64/scheduler.h"

#define RUN_STACK_SIZE   (64 * 1024)
#define RUN_MAX_LAUNCHES 8

typedef struct {
    bool     in_use;
    uint64_t entry;
    void*    stack;
    uint64_t stack_size;
} run_launch_slot_t;

static run_launch_slot_t g_run_launches[RUN_MAX_LAUNCHES];
static spinlock_t g_run_launch_lock = SPINLOCK_INIT;

static uint32_t run_read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

/*
 * Slot lookup is keyed off the process's own name ("<path>#<slot>",
 * see cmd_run below) rather than its PID. The name is set at
 * proc_create() time, before the process is ever enqueued onto a run
 * queue - so by the time this trampoline can possibly execute, the
 * slot it needs to find is guaranteed to already be fully populated.
 * Keying off PID instead would race: the PID is only known AFTER
 * createProcess() returns, by which point the process may already be
 * schedulable on another core.
 */
static void run_process_trampoline(void) {
    process_t* self = getCurrentProcess();

    const char* hash = strrchr(self->name, '#');
    int slot = hash ? (int)minimafs_parse_int(hash + 1) : -1;

    uint64_t entry = 0;
    void* stack = NULL;
    uint64_t stack_size = 0;

    if (slot >= 0 && slot < RUN_MAX_LAUNCHES) {
        uint64_t flags = spinlock_acquire(&g_run_launch_lock);
        if (g_run_launches[slot].in_use) {
            entry       = g_run_launches[slot].entry;
            stack       = g_run_launches[slot].stack;
            stack_size  = g_run_launches[slot].stack_size;
            g_run_launches[slot].in_use = false;
        }
        spinlock_release(&g_run_launch_lock, flags);
    }

    if (!entry || !stack) {
        serial_write_str("run: launch slot not found (this is a bug)\n");
        return;
    }

    void* stack_top = (uint8_t*)stack + stack_size;

    /*
     * Switch to the app's own stack for the call, then switch back
     * before returning - if we didn't restore rsp, and the app's
     * main() ever returns instead of calling mos_exit(), this
     * function's own `ret` (back into proc_trampoline(), see proc.c)
     * would try to pop a return address off the APP's stack instead
     * of the original kernel process stack, corrupting the call chain.
     */
    uint64_t saved_rsp;
    asm volatile(
        "mov %%rsp, %[saved]\n"
        "mov %[newsp], %%rsp\n"
        "call *%[entry]\n"
        "mov %[saved], %%rsp\n"
        : [saved] "=&r" (saved_rsp)
        : [newsp] "r" (stack_top), [entry] "r" (entry)
        : "memory"
    );

    // If we get here, the app's main() returned instead of calling
    // mos_exit(). Falling off the end of this function returns into
    // proc_trampoline() (proc.c), which calls process_exit() for us -
    // same safety net every other kernel process entry point gets.
}

void cmd_run(int argc, const char** argv) {
    if (argc < 2) {
        graphics_write_textr("Usage: run <path-to.run-file>\n");
        return;
    }

    elf_loaded_image_t image;
    uint8_t* elf_data = NULL;
    uint32_t elf_size = 0;
    if (!run_extract_main_elf(argv[1], &elf_data, &elf_size) ||
        !elf_load_buffer(elf_data, elf_size, &image)) {
        graphics_write_textr("run: failed to load ");
        graphics_write_textr(argv[1]);
        graphics_write_textr("\n");
        return;
    }

    void* stack = alloc_unzeroed(RUN_STACK_SIZE);
    if (!stack) {
        graphics_write_textr("run: out of memory allocating stack\n");
        elf_unload(&image);
        return;
    }

    int slot = -1;
    uint64_t flags = spinlock_acquire(&g_run_launch_lock);
    for (int i = 0; i < RUN_MAX_LAUNCHES; i++) {
        if (!g_run_launches[i].in_use) {
            slot = i;
            g_run_launches[i].in_use     = true;
            g_run_launches[i].entry      = image.entry_point;
            g_run_launches[i].stack      = stack;
            g_run_launches[i].stack_size = RUN_STACK_SIZE;
            break;
        }
    }
    spinlock_release(&g_run_launch_lock, flags);

    if (slot < 0) {
        graphics_write_textr("run: too many programs starting at once\n");
        free_mem(stack);
        elf_unload(&image);
        return;
    }

    char procname[96];
    snprintf(procname, sizeof(procname), "%s#%d", argv[1], slot);

    process_t* proc = createProcess(procname, run_process_trampoline);
    if (!proc) {
        graphics_write_textr("run: failed to create process\n");
        flags = spinlock_acquire(&g_run_launch_lock);
        g_run_launches[slot].in_use = false;
        spinlock_release(&g_run_launch_lock, flags);
        free_mem(stack);
        elf_unload(&image);
        return;
    }

    graphics_write_textr("Started: ");
    graphics_write_textr(argv[1]);
    graphics_write_textr(" (PID ");
    graphics_write_textr_udec(proc->pid);
    graphics_write_textr(")\n");
}

void register_run(void) {
    command_register("run", cmd_run);
}

REGISTER_COMMAND(register_run);