// src/impl/proc/spawn.c - SYS_SPAWN, SYS_WAIT, working directory,
// environment and redirected program output (see spawn.h, syscall.h).
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "x86_64/spawn.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/spinlock.h"
#include "x86_64/waitq.h"
#include "x86_64/allocator.h"
#include "x86_64/minimafs.h"
#include "x86_64/runcommand.h"
#include "x86_64/loader/argblock.h"
#include "prochandler.h"
#include "fspaths.h"
#include "serial.h"
#include "string.h"
#include "time.h"

bool syscall_user_range_ok(const void* ptr, size_t len, bool write);

// Biggest argv + environment (strings and pointers) a program can start with.
#define SPAWN_MAX_BLOCK (64u * 1024u)

/* ================================================================ sinks */

// Program output going into a file. The file is appended to whenever the
// buffer fills and when a writer exits, so a waiting parent finds all of
// it. Appending rewrites the file (MinimaFS has no in-place append), hence
// the large buffer.
#define SINK_CAPACITY (16u * 1024u)

struct stdio_sink {
    volatile uint32_t refs;
    volatile uint32_t busy;        // held while buffering or appending (may sleep)
    volatile uint64_t holder_pid;  // who holds `busy`
    uint32_t used;
    uint8_t* buffer;
    struct stdio_sink* next_orphan;
    char path[MINIMAFS_MAX_PATH];
};

// Sinks whose last writer was reaped with output still buffered; written
// out from process context (SYS_WAIT, SYS_SPAWN, any exit).
static spinlock_t g_orphan_lock = SPINLOCK_INIT;
static stdio_sink_t* g_orphans;

// True when `pid` can no longer be executing: gone, or ended and off its CPU.
static bool pid_is_dead(uint64_t pid) {
    process_t* p = findProcessByPID(pid);
    if (!p) return true;
    return (p->state == PROCESS_ZOMBIE || p->state == PROCESS_TERMINATED) && !p->on_cpu;
}

static void sink_lock(stdio_sink_t* s) {
    uint64_t me = current_process ? current_process->pid : 0;
    while (__sync_lock_test_and_set(&s->busy, 1)) {
        // A writer that was killed in the middle of a write (it slept in
        // the disk driver) never releases the sink: take it over, or
        // everyone else writing to it would wait for ever.
        uint64_t holder = s->holder_pid;
        if (holder && holder != me && pid_is_dead(holder) &&
            __sync_bool_compare_and_swap(&s->holder_pid, holder, me))
            return;
        sleep(1);
    }
    s->holder_pid = me;
}

static void sink_unlock(stdio_sink_t* s) {
    s->holder_pid = 0;
    __sync_lock_release(&s->busy);
}

// Caller holds the sink. Disk I/O.
static void sink_flush_locked(stdio_sink_t* s) {
    if (!s->used) return;
    if (!minimafs_append_file(s->path, s->buffer, s->used)) {
        serial_write_str("spawn: could not append program output to ");
        serial_write_str(s->path);
        serial_write_str("\n");
    }
    s->used = 0;
}

static void sink_free(stdio_sink_t* s) {
    if (s->buffer) free_mem(s->buffer);
    free_mem(s);
}

// Creates `resolved` afresh (an existing file is replaced) and a sink for it.
static stdio_sink_t* sink_create(const char* resolved) {
    if (minimafs_exists(resolved)) {
        if (minimafs_is_dir(resolved) || !minimafs_delete_file(resolved)) return NULL;
    }
    if (!minimafs_create_file(resolved, "text", "txt")) return NULL;
    stdio_sink_t* s = (stdio_sink_t*)alloc(sizeof(stdio_sink_t));
    if (!s) return NULL;
    s->buffer = (uint8_t*)alloc_unzeroed(SINK_CAPACITY);
    if (!s->buffer) { free_mem(s); return NULL; }
    s->refs = 1;
    strncpy(s->path, resolved, sizeof(s->path) - 1);
    return s;
}

static stdio_sink_t* sink_get(stdio_sink_t* s) {
    if (s) __atomic_add_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
    return s;
}

void stdio_sink_put(stdio_sink_t* s) {
    if (!s) return;
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST) != 0) return;
    sink_lock(s);
    sink_flush_locked(s);
    sink_unlock(s);
    sink_free(s);
}

// At reap: no disk I/O allowed, so a last reference with output still
// buffered goes on the orphan list.
static void sink_put_noio(stdio_sink_t* s) {
    if (!s) return;
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST) != 0) return;
    if (!s->used && !s->busy) { sink_free(s); return; }
    uint64_t flags = spinlock_acquire(&g_orphan_lock);
    s->next_orphan = g_orphans;
    g_orphans = s;
    spinlock_release(&g_orphan_lock, flags);
}

static void sink_flush_orphans(void) {
    if (!g_orphans) return;
    uint64_t flags = spinlock_acquire(&g_orphan_lock);
    stdio_sink_t* list = g_orphans;
    g_orphans = NULL;
    spinlock_release(&g_orphan_lock, flags);
    while (list) {
        stdio_sink_t* next = list->next_orphan;
        sink_lock(list);
        sink_flush_locked(list);
        sink_unlock(list);
        sink_free(list);
        list = next;
    }
}

int proc_stdio_write(process_t* proc, int fd, const char* buf, uint64_t len) {
    process_t* owner = proc_mem_owner(proc);
    if (!owner || (fd != 1 && fd != 2)) return -1;
    stdio_sink_t* s = fd == 1 ? owner->out_sink : owner->err_sink;
    if (!s) return -1;
    if (!len) return 1;
    if (!syscall_user_range_ok(buf, len, false)) return 0;
    sink_lock(s);
    while (len) {
        uint32_t room = SINK_CAPACITY - s->used;
        uint32_t n = len < room ? (uint32_t)len : room;
        memcpy(s->buffer + s->used, buf, n);
        s->used += n;
        buf += n;
        len -= n;
        if (s->used == SINK_CAPACITY) sink_flush_locked(s);
    }
    sink_unlock(s);
    return 1;
}

/* ===================================================== working directory */

// Guards every program's cwd string: SYS_CHDIR replaces it while other
// threads of the program may be resolving paths against it.
static spinlock_t g_cwd_lock = SPINLOCK_INIT;

bool proc_resolve_path(process_t* proc, const char* input, char* resolved) {
    if (!input || !resolved) return false;
    process_t* owner = proc_mem_owner(proc);
    uint64_t flags = spinlock_acquire(&g_cwd_lock);
    const char* base = (owner && owner->cwd) ? owner->cwd : fs_get_current_directory();
    bool ok = fs_resolve_path_from(base, input, resolved);
    spinlock_release(&g_cwd_lock, flags);
    return ok;
}

// A copy of `owner`'s working directory (kernel heap), or NULL when it uses
// the terminal's.
static char* cwd_copy(process_t* owner) {
    if (!owner) return NULL;
    char* copy = NULL;
    uint64_t flags = spinlock_acquire(&g_cwd_lock);
    if (owner->cwd) {
        size_t len = strlen(owner->cwd);
        // alloc under a spinlock is fine: the kernel allocator has its own.
        copy = (char*)alloc_unzeroed(len + 1);
        if (copy) memcpy(copy, owner->cwd, len + 1);
    }
    spinlock_release(&g_cwd_lock, flags);
    return copy;
}

// Length of a NUL-terminated string in the calling program's memory, at
// most `max` bytes; false when it is not entirely the program's or longer.
static bool user_strlen(const char* s, size_t max, size_t* out_len) {
    if (!s) return false;
    for (size_t i = 0; i < max; i++) {
        uintptr_t a = (uintptr_t)s + i;
        if ((i == 0 || (a & 0xFFFu) == 0) && !syscall_user_range_ok((const void*)a, 1, false))
            return false;
        if (s[i] == '\0') { if (out_len) *out_len = i; return true; }
    }
    return false;
}

uint64_t proc_chdir(const char* path) {
    process_t* owner = proc_mem_owner(current_process);
    if (!owner || owner->privilege != PROC_PRIVILEGE_USER) return SYS_ERR_PERM;
    if (!user_strlen(path, MINIMAFS_MAX_PATH, NULL)) return SYS_ERR_INVAL;
    char* resolved = (char*)alloc_unzeroed(MINIMAFS_MAX_PATH);
    if (!resolved) return SYS_ERR_NOMEM;
    if (!proc_resolve_path(owner, path, resolved)) { free_mem(resolved); return SYS_ERR_INVAL; }
    // The drive root is always a directory; anything else must exist as one.
    const char* colon = strchr(resolved, ':');
    bool root = colon && colon[1] == '/' && colon[2] == '\0';
    if (!root && !minimafs_is_dir(resolved)) { free_mem(resolved); return SYS_ERR_NOTFOUND; }
    uint64_t flags = spinlock_acquire(&g_cwd_lock);
    char* old = owner->cwd;
    owner->cwd = resolved;
    spinlock_release(&g_cwd_lock, flags);
    if (old) free_mem(old);
    return SYS_SUCCESS;
}

uint64_t proc_getcwd(char* out, uint64_t capacity) {
    if (!out || capacity == 0) return SYS_ERR_INVAL;
    bool user = current_process && current_process->privilege == PROC_PRIVILEGE_USER;
    process_t* owner = proc_mem_owner(current_process);
    uint64_t result = SYS_SUCCESS;
    uint64_t flags = spinlock_acquire(&g_cwd_lock);
    const char* cwd = (owner && owner->cwd) ? owner->cwd : fs_get_current_directory();
    size_t len = strlen(cwd);
    // Only the bytes written must be the program's: callers have always
    // been free to pass a capacity larger than what they need.
    if (len + 1 > capacity || (user && !syscall_user_range_ok(out, len + 1, true)))
        result = SYS_ERR_INVAL;
    else
        memcpy(out, cwd, len + 1);
    spinlock_release(&g_cwd_lock, flags);
    return result;
}

/* ============================================================ exit records */

// What SYS_WAIT reports about a child that is gone. Written when the child
// is reaped, consumed by the parent's SYS_WAIT. The record takes over the
// child's references to its output sinks, so the waiting parent writes out
// whatever the child left buffered (a crashed or killed child never got to)
// before SYS_WAIT returns. Records of a parent that has itself ended are
// dropped; when the table is full an old record goes, and waiting for that
// child then answers NOTFOUND rather than hanging.
#define EXIT_RECORDS 64

typedef struct {
    uint64_t pid;
    uint64_t parent;
    uint32_t exit_code;
    uint32_t flags;
    uint32_t crash_vector;
    stdio_sink_t* out;
    stdio_sink_t* err;
    bool used;
} exit_record_t;

static exit_record_t g_exits[EXIT_RECORDS];
static uint32_t g_exit_next;
static spinlock_t g_exit_lock = SPINLOCK_INIT;
static const char g_child_space = 0;    // wait-queue space for SYS_WAIT sleepers

static bool record_find_locked(uint64_t parent, uint64_t pid, uint32_t* index) {
    for (uint32_t i = 0; i < EXIT_RECORDS; i++) {
        if (g_exits[i].used && g_exits[i].parent == parent &&
            (pid == SYS_WAIT_ANY || g_exits[i].pid == pid)) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

static bool record_take(uint64_t parent, uint64_t pid, exit_record_t* out) {
    uint32_t i;
    uint64_t flags = spinlock_acquire(&g_exit_lock);
    bool found = record_find_locked(parent, pid, &i);
    if (found) {
        *out = g_exits[i];
        g_exits[i].used = false;
    }
    spinlock_release(&g_exit_lock, flags);
    return found;
}

typedef struct {
    uint64_t parent;
    uint64_t pid;
} wait_ctx_t;

static bool wait_should_sleep(void* ctx) {
    wait_ctx_t* w = (wait_ctx_t*)ctx;
    uint64_t flags = spinlock_acquire(&g_exit_lock);
    bool found = record_find_locked(w->parent, w->pid, NULL);
    spinlock_release(&g_exit_lock, flags);
    return !found;
}

// A child of `parent` (matching pid, or any) that has not been reaped yet.
static bool child_exists(uint64_t parent, uint64_t pid) {
    bool found = false;
    uint64_t flags = scheduler_lock();
    for (process_t* p = proc_list_head; p; p = p->next) {
        if (p->parent_pid == parent && p->wait_record && !p->mem_owner &&
            (pid == SYS_WAIT_ANY || p->pid == pid)) {
            found = true;
            break;
        }
    }
    scheduler_unlock(flags);
    return found;
}

static void sink_flush_now(stdio_sink_t* s);

// Finishes a wait with a record taken from the table: the child's output
// is on disk before the parent goes on.
static uint64_t wait_finish(exit_record_t* r, syscall_wait_t* out) {
    sink_flush_now(r->out);
    if (r->err != r->out) sink_flush_now(r->err);
    stdio_sink_put(r->out);
    stdio_sink_put(r->err);
    sink_flush_orphans();
    if (out) {
        out->exit_code = r->exit_code;
        out->flags = r->flags;
        out->crash_vector = r->crash_vector;
        out->reserved = 0;
    }
    return r->pid;
}

uint64_t proc_wait_child(uint64_t pid, uint64_t flags, syscall_wait_t* out) {
    process_t* owner = proc_mem_owner(current_process);
    if (!owner || owner->privilege != PROC_PRIVILEGE_USER) return SYS_ERR_PERM;
    if (out && !syscall_user_range_ok(out, sizeof(*out), true)) return SYS_ERR_INVAL;
    if (flags & ~(uint64_t)SYS_WAIT_NOHANG) return SYS_ERR_INVAL;

    for (;;) {
        exit_record_t r;
        if (record_take(owner->pid, pid, &r)) return wait_finish(&r, out);
        if (!child_exists(owner->pid, pid)) {
            // Reaped between the two looks?
            if (record_take(owner->pid, pid, &r)) return wait_finish(&r, out);
            return SYS_ERR_NOTFOUND;
        }
        if (flags & SYS_WAIT_NOHANG) return SYS_ERR_AGAIN;
        // Woken when a child of ours is reaped; the timeout is only a
        // safety net against a missed wake-up.
        wait_ctx_t ctx = { owner->pid, pid };
        waitq_wait(&g_child_space, owner->pid, wait_should_sleep, &ctx,
                   time_get_uptime_ms() + 250);
    }
}

void proc_spawn_wake_parent(uint64_t parent_pid) {
    if (parent_pid) waitq_wake(&g_child_space, parent_pid, UINT32_MAX);
}

/* ========================================================== exit / reap */

static void sink_flush_now(stdio_sink_t* s) {
    if (!s) return;
    sink_lock(s);
    sink_flush_locked(s);
    sink_unlock(s);
}

void proc_spawn_on_exit(process_t* proc) {
    // Not after a crash: the fault may have hit inside the filesystem with
    // its locks held, and writing a file from here would then hang. The
    // waiting parent (or the next spawn/wait/exit) writes the output out.
    if (!proc || proc->crashed) return;
    if (proc->mem_owner) {
        // A thread: its stack is free from here on.
        process_t* owner = proc->mem_owner;
        uint64_t addr = proc->exit_futex;
        proc->exit_futex = 0;
        if (addr && proc_user_range_contains(owner, (const void*)(uintptr_t)addr, 4)) {
            __atomic_store_n((volatile uint32_t*)(uintptr_t)addr, 0u, __ATOMIC_SEQ_CST);
            futex_wake_for(owner, addr, UINT32_MAX);
        }
        return;
    }
    sink_flush_now(proc->out_sink);
    if (proc->err_sink != proc->out_sink) sink_flush_now(proc->err_sink);
    sink_flush_orphans();
}

// Caller holds g_exit_lock. No disk I/O.
static void record_drop_locked(exit_record_t* r) {
    sink_put_noio(r->out);
    sink_put_noio(r->err);
    r->out = r->err = NULL;
    r->used = false;
}

// Caller holds the scheduler lock (the reap pass does).
static bool pid_alive_locked(uint64_t pid) {
    for (process_t* p = proc_list_head; p; p = p->next) {
        if (p->pid == pid)
            return p->state != PROCESS_ZOMBIE && p->state != PROCESS_TERMINATED;
    }
    return false;
}

uint64_t proc_spawn_reap(process_t* proc) {
    if (!proc) return 0;
    uint64_t wake = 0;
    bool recorded = false;
    if (!proc->mem_owner) {
        uint64_t flags = spinlock_acquire(&g_exit_lock);
        // Nobody can wait for this program's own children any more.
        for (uint32_t i = 0; i < EXIT_RECORDS; i++) {
            if (g_exits[i].used && g_exits[i].parent == proc->pid)
                record_drop_locked(&g_exits[i]);
        }
        if (proc->wait_record && proc->parent_pid && pid_alive_locked(proc->parent_pid)) {
            uint32_t slot = EXIT_RECORDS;
            for (uint32_t i = 0; i < EXIT_RECORDS; i++) {
                if (!g_exits[i].used) { slot = i; break; }
            }
            if (slot == EXIT_RECORDS) {
                // Parents that never wait (mosWM starting apps with
                // SYS_EXEC) fill the table; an old record goes. Said once.
                static bool warned;
                slot = g_exit_next++ % EXIT_RECORDS;
                record_drop_locked(&g_exits[slot]);
                if (!warned) {
                    warned = true;
                    serial_write_str("spawn: exit record table full, dropping old records\n");
                }
            }
            exit_record_t* r = &g_exits[slot];
            r->pid = proc->pid;
            r->parent = proc->parent_pid;
            r->exit_code = proc->exit_code;
            r->flags = (proc->crashed ? SYS_WAIT_CRASHED : 0u) |
                       (proc->killed ? SYS_WAIT_KILLED : 0u);
            r->crash_vector = proc->crashed ? proc->crash_vector : 0u;
            r->out = proc->out_sink;         // the references move to the record
            r->err = proc->err_sink;
            r->used = true;
            recorded = true;
            wake = proc->parent_pid;
        }
        spinlock_release(&g_exit_lock, flags);
    }
    if (!recorded) {
        sink_put_noio(proc->out_sink);
        sink_put_noio(proc->err_sink);
    }
    proc->out_sink = proc->err_sink = NULL;
    if (proc->cwd) { free_mem(proc->cwd); proc->cwd = NULL; }
    if (proc->env) { free_mem(proc->env); proc->env = NULL; }
    proc->env_len = 0;
    return wake;
}

/* ================================================================= spawn */

// Packs a NULL-terminated array of user strings into "a\0b\0" in a kernel
// buffer. `count` limits the entries when nonzero (argv); otherwise the
// array ends at its NULL (envp).
static bool pack_user_strings(const char* const* list, uint64_t count, bool counted,
                              uint32_t max_entries, char** out, uint32_t* out_len,
                              uint32_t* out_count) {
    *out = NULL;
    *out_len = 0;
    if (out_count) *out_count = 0;
    if (!list) return true;
    if (counted && count > max_entries) return false;

    size_t total = 0;
    uint32_t n = 0;
    for (;;) {
        if (counted && n == count) break;
        if (n >= max_entries) return false;
        if (!syscall_user_range_ok(&list[n], sizeof(char*), false)) return false;
        const char* s = list[n];
        if (!s) {
            if (counted) return false;
            break;
        }
        size_t len;
        if (!user_strlen(s, SPAWN_MAX_BLOCK, &len)) return false;
        total += len + 1;
        if (total > SPAWN_MAX_BLOCK) return false;
        n++;
    }

    char* buf = (char*)alloc_unzeroed(total ? total : 1);
    if (!buf) return false;
    size_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t len = strlen(list[i]);
        if (at + len + 1 > total) { free_mem(buf); return false; }    // changed meanwhile
        memcpy(buf + at, list[i], len);
        buf[at + len] = '\0';
        at += len + 1;
    }
    *out = buf;
    *out_len = (uint32_t)at;
    if (out_count) *out_count = n;
    return true;
}

// The NUL-separated strings of a packed buffer as an array (kernel heap).
static const char** unpack_strings(const char* packed, uint32_t len, uint32_t count) {
    const char** v = (const char**)alloc_unzeroed((count ? count : 1) * sizeof(char*));
    if (!v) return NULL;
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        v[i] = packed + at;
        while (at < len && packed[at]) at++;
        at++;
    }
    return v;
}

// A sink for a stdout/stderr path of the request (resolved against the
// caller's working directory).
static bool resolve_user_path(process_t* proc, const char* user_path, char* resolved) {
    if (!user_strlen(user_path, MINIMAFS_MAX_PATH, NULL)) return false;
    return proc_resolve_path(proc, user_path, resolved);
}

uint64_t proc_spawn(const syscall_spawn_t* request) {
    process_t* self = current_process;
    process_t* owner = proc_mem_owner(self);
    if (!owner || owner->privilege != PROC_PRIVILEGE_USER) return SYS_ERR_PERM;
    if (!syscall_user_range_ok(request, sizeof(uint64_t), false)) return SYS_ERR_INVAL;
    if (request->size < sizeof(syscall_spawn_t) ||
        !syscall_user_range_ok(request, sizeof(syscall_spawn_t), false))
        return SYS_ERR_INVAL;
    syscall_spawn_t r = *request;
    if (r.flags) return SYS_ERR_INVAL;

    sink_flush_orphans();

    uint64_t result = SYS_ERR_INVAL;
    char* scratch = (char*)alloc_unzeroed(4u * MINIMAFS_MAX_PATH);
    char* argv_packed = NULL;
    char* env = NULL;
    const char** argv = NULL;
    stdio_sink_t* out_sink = NULL;
    stdio_sink_t* err_sink = NULL;
    uint32_t argv_len = 0, argc = 0, env_len = 0;
    if (!scratch) return SYS_ERR_NOMEM;
    char* path = scratch;
    char* cwd = scratch + MINIMAFS_MAX_PATH;
    char* out_path = scratch + 2u * MINIMAFS_MAX_PATH;
    char* err_path = scratch + 3u * MINIMAFS_MAX_PATH;

    // The program.
    if (!resolve_user_path(self, r.path, path)) goto done;
    if (!minimafs_exists(path) || minimafs_is_dir(path)) { result = SYS_ERR_NOTFOUND; goto done; }

    // argv, copied into the kernel so the program cannot change it while
    // the child's stack is built.
    if (r.argv) {
        if (!pack_user_strings(r.argv, r.argc, true, ARGBLOCK_MAX_ARGS,
                               &argv_packed, &argv_len, &argc))
            goto done;
        argv = unpack_strings(argv_packed, argv_len, argc);
        if (!argv) { result = SYS_ERR_NOMEM; goto done; }
    }

    // Environment: the given one, or a copy of the one the caller started with.
    if (r.envp) {
        if (!pack_user_strings(r.envp, 0, false, ARGBLOCK_MAX_ENV, &env, &env_len, NULL))
            goto done;
    } else if (owner->env && owner->env_len) {
        env = (char*)alloc_unzeroed(owner->env_len);
        if (!env) { result = SYS_ERR_NOMEM; goto done; }
        memcpy(env, owner->env, owner->env_len);
        env_len = owner->env_len;
    }
    if ((size_t)argv_len + env_len > SPAWN_MAX_BLOCK) goto done;

    // Working directory: the given one (must be a directory), or the caller's.
    char* inherited_cwd = NULL;
    if (r.cwd) {
        if (!resolve_user_path(self, r.cwd, cwd)) goto done;
        const char* colon = strchr(cwd, ':');
        bool root = colon && colon[1] == '/' && colon[2] == '\0';
        if (!root && !minimafs_is_dir(cwd)) { result = SYS_ERR_NOTFOUND; goto done; }
    } else {
        inherited_cwd = cwd_copy(owner);
    }

    // Output: new files, or wherever the caller's output goes.
    if (r.stdout_path) {
        if (!resolve_user_path(self, r.stdout_path, out_path)) { free_mem(inherited_cwd); goto done; }
        out_sink = sink_create(out_path);
        if (!out_sink) { free_mem(inherited_cwd); result = SYS_ERR_GENERIC; goto done; }
    } else {
        out_sink = sink_get(owner->out_sink);
    }
    if (r.stderr_path) {
        if (!resolve_user_path(self, r.stderr_path, err_path)) { free_mem(inherited_cwd); goto done; }
        if (r.stdout_path && strcmp(err_path, out_path) == 0) {
            err_sink = sink_get(out_sink);
        } else {
            err_sink = sink_create(err_path);
            if (!err_sink) { free_mem(inherited_cwd); result = SYS_ERR_GENERIC; goto done; }
        }
    } else {
        err_sink = sink_get(owner->err_sink);
    }

    run_launch_t launch;
    memset(&launch, 0, sizeof(launch));
    launch.path = path;
    launch.argc = argc;
    launch.argv = argv;
    launch.env = env;
    launch.env_len = env_len;
    launch.cwd = r.cwd ? cwd : inherited_cwd;
    launch.out_sink = out_sink;          // run_launch_ex takes these references
    launch.err_sink = err_sink;
    launch.parent_pid = owner->pid;
    launch.wait_record = true;
    out_sink = err_sink = NULL;
    process_t* child = run_launch_ex(&launch);
    if (inherited_cwd) free_mem(inherited_cwd);
    result = child ? child->pid : SYS_ERR_GENERIC;

done:
    if (out_sink) stdio_sink_put(out_sink);
    if (err_sink) stdio_sink_put(err_sink);
    if (argv) free_mem((void*)argv);
    if (argv_packed) free_mem(argv_packed);
    if (env) free_mem(env);
    free_mem(scratch);
    return result;
}

uint64_t proc_exec_inherit(const char* path, int extra_argc, const char** extra_argv) {
    process_t* owner = proc_mem_owner(current_process);
    if (!owner) return SYS_ERR_PERM;
    if (extra_argc < 0 || (uint32_t)extra_argc + 1u > ARGBLOCK_MAX_ARGS) return SYS_ERR_INVAL;

    const char** argv = (const char**)alloc_unzeroed(((size_t)extra_argc + 1u) * sizeof(char*));
    if (!argv) return SYS_ERR_NOMEM;
    argv[0] = path;
    for (int i = 0; i < extra_argc; i++) argv[1 + i] = extra_argv[i] ? extra_argv[i] : "";

    run_launch_t launch;
    memset(&launch, 0, sizeof(launch));
    launch.path = path;
    launch.argc = (uint32_t)extra_argc + 1u;
    launch.argv = argv;
    launch.env = owner->env;             // copied by run_launch_ex
    launch.env_len = owner->env_len;
    char* cwd = cwd_copy(owner);
    launch.cwd = cwd;
    launch.out_sink = sink_get(owner->out_sink);
    launch.err_sink = sink_get(owner->err_sink);
    launch.parent_pid = owner->pid;
    launch.wait_record = true;
    launch.terminal_foreground = true;   // what SYS_EXEC always did
    process_t* child = run_launch_ex(&launch);
    if (cwd) free_mem(cwd);
    free_mem((void*)argv);
    return child ? child->pid : SYS_ERR_GENERIC;
}
