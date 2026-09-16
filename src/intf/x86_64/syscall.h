#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_GETPID  3
#define SYS_UPTIME  4
#define SYS_SLEEP   5
#define SYS_OPEN    6
#define SYS_READ    7
#define SYS_CLOSE   8
#define SYS_EXISTS  9
#define SYS_IS_DIR  10
#define SYS_GETTIME 11
#define SYS_SEEK    12
#define SYS_SIZE    13
#define SYS_MKDIR   14
#define SYS_GRAPHICS 15
#define SYS_MANAGER  16
#define SYS_USB      17

// ===========================================
// MinimaFS extension syscalls
// ===========================================
// These operate on the same "fd" concept SYS_OPEN already established -
// the raw minimafs_file_handle_t* returned by SYS_OPEN, cast to a
// register value. No new handle table, no new lifetime rules: SYS_CLOSE
// still owns closing it.
#define SYS_FWRITE        18  // write to an open MinimaFS handle (not stdout - see SYS_WRITE)
#define SYS_LISTDIR       19  // list a directory into a syscall_dirent_t[] buffer
#define SYS_DELETE        20  // delete a file
#define SYS_RMDIR         21  // remove an (empty) directory
#define SYS_GET_METADATA  22  // read a file's MinimaFS metadata
#define SYS_TELL          23  // current read/write position of an open handle
#define SYS_EOF           24  // true if an open handle is at EOF

// ===========================================
// MPKG (.mpkg archive) syscall
// ===========================================
#define SYS_PKG           25
#define SYS_REGISTER_CLEANUP 28

#define SYS_O_RDONLY 0

#define SYS_SUCCESS       0
#define SYS_ERR_GENERIC  ((uint64_t)-1)
#define SYS_ERR_BADFD    ((uint64_t)-2)
#define SYS_ERR_NOTFOUND ((uint64_t)-3)
#define SYS_ERR_INVAL    ((uint64_t)-4)
// Returned when a PROC_PRIVILEGE_USER process (see process_privilege_t
// in proc.h) calls a syscall/operation reserved for kernel-privilege
// callers. See the syscall_user_may_call() privilege table in
// syscall.c for exactly which operations this applies to, and why.
#define SYS_ERR_PERM     ((uint64_t)-5)

typedef enum {
    SYS_GRAPHICS_GET_WIDTH = 1,
    SYS_GRAPHICS_GET_HEIGHT,
    SYS_GRAPHICS_CLEAR,
    SYS_GRAPHICS_PIXEL,
    SYS_GRAPHICS_LINE,
    SYS_GRAPHICS_CIRCLE,
    SYS_GRAPHICS_FILL_CIRCLE,
    SYS_GRAPHICS_RECTANGLE,
    SYS_GRAPHICS_FILL_RECTANGLE,
    SYS_GRAPHICS_TRIANGLE,
    SYS_GRAPHICS_FILL_TRIANGLE,
    SYS_GRAPHICS_ELLIPSE,
    SYS_GRAPHICS_TEXT,
    SYS_GRAPHICS_MEASURE_TEXT,
    SYS_GRAPHICS_SET_RESOLUTION,
    SYS_GRAPHICS_TERMINAL_CLEAR,
} syscall_graphics_op_t;

typedef struct {
    uint64_t op;
    int64_t args[10];
    const char* text;
    uint32_t* out_width;
    uint32_t* out_height;
} syscall_graphics_request_t;

typedef enum {
    SYS_MANAGER_GPU = 1,
    SYS_MANAGER_AUDIO,
    SYS_MANAGER_STORAGE,
    SYS_MANAGER_USB,
} syscall_manager_id_t;

typedef enum {
    SYS_MANAGER_INIT = 1,
    SYS_MANAGER_UPDATE,
    SYS_MANAGER_START,
    SYS_MANAGER_HAS_DATA,
    SYS_MANAGER_SET_SAMPLE_RATE,
} syscall_manager_op_t;

typedef enum {
    SYS_USB_INIT = 1,
    SYS_USB_POLL,
    SYS_USB_HAS_KEYBOARD,
    SYS_USB_KEY_TO_ASCII,
    SYS_USB_KEY_IS_PRESSED,
    SYS_USB_KEYBOARD_INFO,
} syscall_usb_op_t;

typedef struct {
    uint8_t address;
    uint8_t port;
    uint8_t state;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t is_keyboard;
} syscall_usb_keyboard_info_t;

// ===========================================
// MinimaFS extension structures
// ===========================================
//
// Deliberately independent of minimafs_dir_entry_t / minimafs_file_metadata_t
// (kernel-internal structs that are free to keep growing - see minimafs.h) so
// this syscall ABI never has to change shape just because the filesystem's
// internal representation does. Only the fields a user program can actually
// use are exposed here.

#define SYSCALL_DIRENT_NAME_SIZE 64

// type field values, matching minimafs_filetype_t's ordering (kept in sync
// manually - see minimafs.h's minimafs_filetype_t).
#define SYSCALL_DIRENT_TYPE_FILE       0
#define SYSCALL_DIRENT_TYPE_DIR        1
#define SYSCALL_DIRENT_TYPE_EXECUTABLE 2
#define SYSCALL_DIRENT_TYPE_SYMLINK    3

typedef struct {
    char name[SYSCALL_DIRENT_NAME_SIZE];
    uint8_t type;     // SYSCALL_DIRENT_TYPE_*
    uint8_t hidden;
} syscall_dirent_t;

typedef struct {
    char filetype[64];
    char fileformat[16];
    uint32_t data_length;
    uint8_t runnable;
    uint8_t hidden;
    uint64_t entrypoint;
    char created_date[32];
    char last_changed[32];
} syscall_file_metadata_t;

// ===========================================
// MPKG (.mpkg archive) syscall structures
// ===========================================

typedef enum {
    SYS_PKG_UNZIP = 1,   // extract `path` (archive) into `extra` (target dir)
    SYS_PKG_ZIP,         // archive `path` (file/dir) using `extra` (algorithm, may be NULL)
    SYS_PKG_INFO,        // inspect `path` (archive), report entry count via out_count
} syscall_pkg_op_t;

typedef struct {
    uint64_t op;               // syscall_pkg_op_t
    const char* path;          // archive path (unzip/info) or source path (zip)
    const char* extra;         // unzip: target dir. zip: algorithm ("lzss"|"store", NULL=default). info: unused.
    char* out_path;             // zip only: buffer to receive the produced "<source>.mpkg" path
    uint32_t out_path_size;     // zip only: size of out_path buffer
    uint32_t* out_count;        // unzip: installed-entry count. info: total entry count.
    uint32_t* out_failed;       // unzip only: failed-entry count
} syscall_pkg_request_t;

// Layout MUST match the push order in syscall_isr.asm exactly.
typedef struct __attribute__((packed)) {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} syscall_regs_t;

#define SYS_SYSINFO 26

// ===========================================
// PROCESS LIST (SYS_PSLIST)
// ===========================================
//
// Deliberately independent of process_t (proc.h) - only the fields a
// user program can actually use are exposed, kept separate so the
// kernel struct can keep growing without breaking this ABI. Call with
// mos_pslist(buffer, max_entries) - see minimalos.h.
#define SYS_PSLIST 27

#define SYSCALL_PROCESS_NAME_SIZE 128   // matches MAX_PROCESS_NAME_LEN in proc.h

#define SYSCALL_PROC_STATE_READY      0
#define SYSCALL_PROC_STATE_RUNNING    1
#define SYSCALL_PROC_STATE_WAITING    2
#define SYSCALL_PROC_STATE_PAUSED     3
#define SYSCALL_PROC_STATE_ZOMBIE     4
#define SYSCALL_PROC_STATE_TERMINATED 5

#define SYSCALL_PROC_PRIV_KERNEL 0
#define SYSCALL_PROC_PRIV_USER   1

typedef struct {
    uint64_t pid;
    char name[SYSCALL_PROCESS_NAME_SIZE];
    uint8_t state;       // SYSCALL_PROC_STATE_*
    uint8_t privilege;   // SYSCALL_PROC_PRIV_*
    uint32_t sched_cpu;
    uint32_t sched_level;
} syscall_process_info_t;

typedef enum {
    SYS_SYSINFO_CPU_USAGE       = 0x01, // avg CPU usage % across all online cores
    SYS_SYSINFO_RAM_TOTAL       = 0x02, // total RAM, bytes
    SYS_SYSINFO_RAM_USED        = 0x03, // used RAM (heap + PMM), bytes
    SYS_SYSINFO_RAM_FREE        = 0x04, // free RAM, bytes
    SYS_SYSINFO_UPTIME_MS       = 0x05,
    SYS_SYSINFO_PROCESS_COUNT   = 0x06,
    SYS_SYSINFO_CORE_COUNT      = 0x07, // online core count
    SYS_SYSINFO_HEAP_USED       = 0x08, // allocator (kernel heap), bytes
    SYS_SYSINFO_HEAP_FREE       = 0x09,
    SYS_SYSINFO_PMM_USED_PAGES  = 0x0A, // 4KB physical pages
    SYS_SYSINFO_PMM_FREE_PAGES  = 0x0B,
    // 0x0C-0x0F reserved for future global stats

    // Per-core usage: op = SYS_SYSINFO_CORE_USAGE_BASE + core_id.
    // Deliberately last/open-ended since core count can vary between
    // boots (SMP bring-up may bring up fewer cores than expected).
    SYS_SYSINFO_CORE_USAGE_BASE = 0x10,
} syscall_sysinfo_op_t;

// Called by isr_syscall_wrapped (syscall_isr.asm) for every `int 0x80`.
// regs->rax = syscall number in, return value out. Args in
// rdi/rsi/rdx (SysV-ish - matches a future `syscall`-instruction ABI
// too, if ring3 ever lands). Return values are in rax; failures are
// negative values represented as uint64_t.
//
// Before dispatching, this enforces a privilege check for
// PROC_PRIVILEGE_USER callers - see syscall_user_may_call() in
// syscall.c. User processes enter through the DPL3 int 0x80 gate;
// syscall policy remains a separate capability boundary.
void syscall_dispatch(syscall_regs_t* regs);

#endif // SYSCALL_H
