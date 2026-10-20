#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "fgpu_abi.h"

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
#define SYS_MOUSE         29

// ===========================================
// NETWORK (SYS_NET), USER HEAP (SYS_HEAP), CSPRNG (SYS_RANDOM)
// ===========================================
// SYS_NET exposes the kernel's existing net stack (src/impl/net/*) to
// user processes. It is a dispatch syscall in the same shape as
// SYS_GRAPHICS / SYS_PKG: rdi points at a syscall_net_request_t.
//
// Connections are referred to by small integer handles owned by
// net_syscall.c, NOT by raw tcp_conn_t* - the kernel struct is free to
// keep changing shape without breaking this ABI, and the handle table
// gives the kernel a place to validate what userland hands back.
#define SYS_NET           30
// Userland heap. Every process currently shares the kernel's PML4
// (see proc_create()), so this is a thin shim over the kernel
// allocator rather than a separate per-process address space.
#define SYS_HEAP          31
// Kernel entropy source - needed for anything generating session keys.
#define SYS_RANDOM        32
// Launch another program (a .run bundle), the same way the terminal's
// `run` command does. rdi = path, rsi = const char** extra argv,
// rdx = extra argc. Returns the new pid.
//
// A .run program could already do anything the kernel can - it shares
// the kernel's address space - so this grants no new authority; it
// just makes the existing capability reachable. It is what makes an
// [Install.<platform>] `run=` directive executable without a shell.
#define SYS_EXEC          33
// Create a new (empty) MinimaFS file. SYS_OPEN only ever opens an
// existing file - minimafs_open() has no create-on-missing behaviour -
// so without this a user program can read and append but can never
// bring a new file into existence, which is the whole job of a package
// manager. rdi = path, rsi = filetype (e.g. "binary"), rdx = format
// (e.g. "bin"); both default sensibly when NULL.
#define SYS_CREATE        34
#define SYS_GETCWD        35
#define SYS_INPUT_READ    36
#define SYS_WINDOW        37
// SYS_GPU (40) and its structures: see fgpu_abi.h

#define SYS_O_RDONLY 0
// Anything non-zero opens writable; SYS_OPEN maps flags==SYS_O_RDONLY
// to minimafs_open(path, read_only=true).
#define SYS_O_RDWR   1

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
// The (non-reentrant) network stack is already in use by another
// process/core; the caller should retry rather than treat this as a
// hard failure. See net_syscall.c.
#define SYS_ERR_BUSY     ((uint64_t)-6)
// Non-blocking call with nothing to report yet (SYS_NET_TCP_ACCEPT with
// an empty backlog). Not an error: call again.
#define SYS_ERR_AGAIN    ((uint64_t)-7)
// Out of memory (kernel heap or video memory).
#define SYS_ERR_NOMEM    ((uint64_t)-8)

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
    SYS_GRAPHICS_ACQUIRE,
    SYS_GRAPHICS_RELEASE,
    SYS_GRAPHICS_OWNER,
    SYS_GRAPHICS_BLIT,
    SYS_GRAPHICS_PRESENT,
    SYS_GRAPHICS_INFO,
} syscall_graphics_op_t;

#define SYS_GRAPHICS_ABI_VERSION 1u
#define SYS_GRAPHICS_BLIT_OPAQUE 1u
#define SYS_GRAPHICS_BLIT_ALPHA 2u
#define SYS_GRAPHICS_PIXEL_FORMAT_ARGB8888 1u
#define SYSCALL_GFX_MAX_DIMENSION 16384u
#define SYSCALL_GFX_MAX_BUFFER_BYTES (64u * 1024u * 1024u)

#define SYS_INPUT_ABI_VERSION 1u
#define SYSCALL_INPUT_KEY_DOWN 1u
#define SYSCALL_INPUT_KEY_UP 2u
#define SYSCALL_INPUT_MOUSE_MOVE 3u
#define SYSCALL_INPUT_MOUSE_BUTTON 4u
#define SYSCALL_INPUT_MOUSE_WHEEL 5u
#define SYSCALL_INPUT_MOD_SHIFT (1u << 0)
#define SYSCALL_INPUT_MOD_CTRL (1u << 1)
#define SYSCALL_INPUT_MOD_ALT (1u << 2)
#define SYSCALL_INPUT_BTN_LEFT (1u << 0)
#define SYSCALL_INPUT_BTN_RIGHT (1u << 1)
#define SYSCALL_INPUT_BTN_MIDDLE (1u << 2)
#define SYSCALL_INPUT_QUEUE_CAPACITY 256u
#define SYSCALL_INPUT_MAX_READ 256u
#define SYSCALL_INPUT_MAX_TIMEOUT_MS 5000u

typedef struct {
    uint8_t type;
    uint8_t key;
    uint8_t mods;
    uint8_t buttons;
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    uint32_t time_ms;
} syscall_input_event_t;

typedef struct {
    uint32_t size;
    uint32_t version;
    syscall_input_event_t* events;
    uint32_t capacity;
    uint32_t timeout_ms;
    uint64_t* dropped_count;
    uint32_t reserved[4];
} syscall_input_read_request_t;

typedef struct {
    uint32_t size;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t pixel_format;
    uint8_t bytes_per_pixel;
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
    uint8_t reserved;
} syscall_graphics_info_t;

typedef struct {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t src_w;
    uint32_t src_h;
    uint32_t src_pitch;
    const uint32_t* src;
    int32_t sx;
    int32_t sy;
    int32_t sw;
    int32_t sh;
    int32_t dx;
    int32_t dy;
    uint32_t reserved[4];
} syscall_blit_request_t;

typedef struct {
    uint64_t op;
    int64_t args[10];
    const char* text;
    uint32_t* out_width;
    uint32_t* out_height;
    const syscall_blit_request_t* blit;
    syscall_graphics_info_t* info;
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
// MOUSE (SYS_MOUSE)
// ===========================================

typedef enum {
    SYS_MOUSE_INIT = 1,
    SYS_MOUSE_POLL,
    SYS_MOUSE_HAS_MOUSE,
    SYS_MOUSE_GET_STATE,   // consumes accumulated delta since last call
} syscall_mouse_op_t;

#define SYSCALL_MOUSE_BTN_LEFT   (1 << 0)
#define SYSCALL_MOUSE_BTN_RIGHT  (1 << 1)
#define SYSCALL_MOUSE_BTN_MIDDLE (1 << 2)

typedef struct {
    int32_t dx;      // relative movement since last GET_STATE call
    int32_t dy;
    int32_t wheel;   // relative scroll since last GET_STATE call
    uint8_t buttons; // SYSCALL_MOUSE_BTN_* bitmask, current state
} syscall_mouse_state_t;

// ===========================================
// Window manager (SYS_WINDOW)
// ===========================================
#define SYS_WINDOW_ABI_VERSION 1u
#define SYSCALL_WINDOW_MAX_TITLE 64u
#define SYSCALL_WINDOW_MAX_WINDOWS 64u
#define SYSCALL_WINDOW_MAX_EVENTS 16u

typedef enum {
    SYS_WINDOW_CREATE = 1,
    SYS_WINDOW_CLOSE,
    SYS_WINDOW_SET_BOUNDS,
    SYS_WINDOW_SET_TITLE,
    SYS_WINDOW_SET_FLAGS,
    SYS_WINDOW_SET_STYLE,
    SYS_WINDOW_PRESENT,
    SYS_WINDOW_GET_INFO,
    SYS_WINDOW_LIST,
    SYS_WINDOW_GET_FRAME,
    SYS_WINDOW_NEXT_EVENT,
    SYS_WINDOW_POST_EVENT,
    SYS_WINDOW_REGISTER_MANAGER,
} syscall_window_op_t;

#define SYSCALL_WINDOW_VISIBLE      (1u << 0)
#define SYSCALL_WINDOW_RESIZABLE    (1u << 1)
#define SYSCALL_WINDOW_FULLSCREEN   (1u << 2)
#define SYSCALL_WINDOW_MAXIMIZED   (1u << 3)
#define SYSCALL_WINDOW_MINIMIZED   (1u << 4)
#define SYSCALL_WINDOW_FOCUSED     (1u << 5)

typedef enum {
    SYSCALL_WINDOW_EVENT_RESIZED = 1,
    SYSCALL_WINDOW_EVENT_CLOSE_REQUEST,
    SYSCALL_WINDOW_EVENT_MINIMIZED,
    SYSCALL_WINDOW_EVENT_MAXIMIZED,
    SYSCALL_WINDOW_EVENT_RESTORED,
    SYSCALL_WINDOW_EVENT_FOCUS_GAINED,
    SYSCALL_WINDOW_EVENT_FOCUS_LOST,
    SYSCALL_WINDOW_EVENT_KEY,
    SYSCALL_WINDOW_EVENT_MOUSE,
    SYSCALL_WINDOW_EVENT_MOUSE_WHEEL,
} syscall_window_event_type_t;

typedef struct {
    uint32_t title_bar;
    uint32_t title_text;
    uint32_t close_button;
    uint32_t minimize_button;
    uint32_t maximize_button;
    uint32_t close_icon;
    uint32_t minimize_icon;
    uint32_t maximize_icon;
    uint32_t border;
    uint32_t content_background;
} syscall_window_style_t;

typedef struct {
    uint32_t id;
    uint32_t parent_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t frame_version;
    uint64_t owner_pid;
    char title[SYSCALL_WINDOW_MAX_TITLE];
    syscall_window_style_t style;
} syscall_window_info_t;

typedef struct {
    uint32_t id;
    uint32_t type;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t key;
    uint32_t buttons;
    int32_t wheel;
    uint8_t mods;
    uint8_t reserved[3];
    uint32_t time_ms;
} syscall_window_event_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t* pixels;
} syscall_window_frame_t;

typedef struct {
    uint32_t size;
    uint32_t version;
    uint32_t op;
    uint32_t id;
    uint32_t parent_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    char title[SYSCALL_WINDOW_MAX_TITLE];
    const uint32_t* pixels;
    uint32_t pitch;
    syscall_window_info_t* info;
    syscall_window_info_t* windows;
    uint32_t capacity;
    syscall_window_frame_t* frame;
    syscall_window_event_t* event;
    syscall_window_style_t style;
} syscall_window_request_t;

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


// ===========================================
// NETWORK (SYS_NET) structures
// ===========================================
//
// All IPv4 addresses in this ABI are host-order uint32_t (the same
// representation ip.c uses internally and that ip_parse() produces),
// NOT network order. Ports are host order too.

typedef enum {
    SYS_NET_STATUS = 1,     // fill out_status; no side effects
    SYS_NET_DHCP,           // kernel-only: mutates global interface config
    SYS_NET_RESOLVE,        // host -> out_ip (uses timeout_ms)
    SYS_NET_POLL,           // pump the NIC once; cheap, safe to call in a loop

    SYS_NET_TCP_CONNECT = 0x10, // ip/port/timeout_ms -> out_handle
    SYS_NET_TCP_SEND,           // handle/buf/len -> bytes actually sent
    SYS_NET_TCP_RECV,           // handle/buf/len -> bytes (0 = nothing yet)
    SYS_NET_TCP_CLOSE,          // handle
    SYS_NET_TCP_STATE,          // handle -> SYSCALL_NET_TCP_*

    // Passive open (server side). Listener handles and connection
    // handles are separate namespaces: a listener handle is only ever
    // valid for ACCEPT/UNLISTEN, a connection handle for the TCP_* ops
    // above.
    SYS_NET_TCP_LISTEN,         // local_port, len = backlog (0 = default) -> out_handle
    SYS_NET_TCP_ACCEPT,         // handle = listener; NON-BLOCKING. SYS_SUCCESS fills
                                // out_handle/out_from_ip/out_from_port, SYS_ERR_AGAIN = none yet
    SYS_NET_TCP_UNLISTEN,       // handle = listener
    SYS_NET_TCP_HANDOFF,        // handle = connection, len = pid to give it to (see below)

    SYS_NET_UDP_BIND = 0x20,    // local_port -> out_handle
    SYS_NET_UDP_UNBIND,         // handle
    SYS_NET_UDP_RECV,           // handle/buf/len -> bytes, fills out_from_*
    SYS_NET_UDP_SEND,           // ip/port/local_port/buf/len

    // TLS-style sessions (ktls). Separate handle namespace from TCP.
    SYS_NET_TLS_CONNECT = 0x30, // ip/port/timeout_ms -> out_handle. TCP up + hello sent;
                                // handshake completes in the background (poll TLS_STATE)
    SYS_NET_TLS_ACCEPT,         // handle = TCP conn you own -> out_handle. The TCP handle
                                // becomes unusable for raw ops; closing the TLS handle closes it
    SYS_NET_TLS_STATE,          // handle -> SYSCALL_NET_TLS_*  (also advances the handshake)
    SYS_NET_TLS_SEND,           // handle/buf/len -> bytes (<= 4096 per call), SYS_ERR_AGAIN while handshaking
    SYS_NET_TLS_RECV,           // handle/buf/len -> bytes, 0 = nothing yet, SYS_ERR_NOTFOUND = closed/dead
    SYS_NET_TLS_CLOSE,          // handle
} syscall_net_op_t;

// Handle ownership. Every connection and listener handle belongs to the
// process that created (or accepted) it. When that process is gone
// (exited, crashed, or was killed) the kernel closes its handles the
// next time any process makes a handle-allocating call, so a dead
// program cannot exhaust the table. A server that hands an accepted
// connection to a child process calls SYS_NET_TCP_HANDOFF with the
// child's pid right after starting it (only the current owner may hand a
// handle over, which also stops a stale handle number that has since
// been reused from being stolen). Until then the accepting process still
// owns it. The child does not need to do anything to receive it: the
// handle number is passed on its command line.

// Mirrors tcp_state_t (net/tcp.h) but kept separate so the kernel enum
// can grow (SYN_RCVD is folded into CONNECTING today) without
// changing what userland sees.
#define SYSCALL_NET_TCP_CLOSED      0
#define SYSCALL_NET_TCP_CONNECTING  1
#define SYSCALL_NET_TCP_ESTABLISHED 2
#define SYSCALL_NET_TCP_CLOSING     3
#define SYSCALL_NET_TCP_PEER_CLOSED 4
#define SYSCALL_NET_TLS_CLOSED       0   /* failed or closed */
#define SYSCALL_NET_TLS_HANDSHAKING  1
#define SYSCALL_NET_TLS_ESTABLISHED  2

// 255.255.255.255 - accepted as a destination by ip_send().
#define SYSCALL_NET_IP_BROADCAST 0xFFFFFFFFu

typedef struct {
    uint8_t  has_driver;    // a NIC driver is registered
    uint8_t  configured;    // an IPv4 address is set (ip_is_configured())
    uint8_t  mac[6];
    uint32_t local_ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
} syscall_net_status_t;

typedef struct {
    uint64_t op;                // syscall_net_op_t
    uint64_t handle;            // tcp/udp handle for ops that take one

    uint32_t ip;                // remote IPv4, host order
    uint16_t port;              // remote port
    uint16_t local_port;        // udp bind / udp send source port
    uint32_t timeout_ms;        // connect / resolve

    void*    buf;               // send source or recv destination
    uint32_t len;               // size of buf

    const char* host;           // SYS_NET_RESOLVE

    uint64_t* out_handle;
    uint32_t* out_ip;
    uint32_t* out_from_ip;      // udp recv: datagram source
    uint16_t* out_from_port;
    syscall_net_status_t* out_status;
} syscall_net_request_t;

// ===========================================
// USER HEAP (SYS_HEAP)
// ===========================================
// rdi = op, rsi = pointer, rdx = size. Returns the allocation address
// as a uint64_t, or one of the SYS_ERR_* values. Addresses that high
// are not valid heap pointers, so the two cannot be confused.
typedef enum {
    SYS_HEAP_ALLOC = 1,     // rdx = size            -> address
    SYS_HEAP_FREE,          // rsi = pointer         -> SYS_SUCCESS
    SYS_HEAP_RESIZE,        // rsi = pointer, rdx = new size -> address
    SYS_HEAP_ALLOC_ZEROED,  // rdx = size            -> address, zeroed
} syscall_heap_op_t;

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
// INTERNET TIME (SYS_TIMESYNC)
// ===========================================
//
// rdi = syscall_timesync_t*. The kernel polls an HTTP(S) address that
// answers with the current Unix time in MILLISECONDS (for example
// https://timeapi.io/api/v1/time/current/unix_ms) and sets the system
// clock, in UTC, from it. CONFIGURE replaces the settings (enabled, url,
// interval_s 5-60); STATUS fills in the result fields.
#define SYS_TIMESYNC 38

#define SYS_TIMESYNC_CONFIGURE 1u
#define SYS_TIMESYNC_STATUS    2u
#define SYS_TIMESYNC_URL_SIZE   192u
#define SYS_TIMESYNC_ERROR_SIZE 64u
#define SYS_TIMESYNC_MIN_INTERVAL 5u
#define SYS_TIMESYNC_MAX_INTERVAL 60u

typedef struct {
    uint32_t op;
    uint32_t enabled;
    uint32_t interval_s;
    uint32_t synced;            // STATUS: 1 once a sync has succeeded
    char url[SYS_TIMESYNC_URL_SIZE];
    uint64_t last_unix_ms;      // STATUS: time from the last good answer
    uint64_t last_sync_age_ms;  // STATUS: how long ago that was
    uint64_t attempts;          // STATUS: requests made since boot
    char error[SYS_TIMESYNC_ERROR_SIZE]; // STATUS: last failure, "" if none
} syscall_timesync_t;

// ===========================================
// RUN A KERNEL COMMAND (SYS_ROOT)
// ===========================================
//
// rdi = syscall_root_t*. Lets a user program (the MOSdesk Terminal) run a
// kernel terminal command as root (like sudo). The kernel checks the password of a
// MOSdesk account itself (0:/programs/mosdesk/accounts.dat) and then gives
// the calling process a ticket for SYS_ROOT_TICKET_MS, so the password is
// asked for once and not for every command.
//   AUTH   username + password -> status 0 and a ticket, or an error
//   RUN    command (needs a ticket) -> starts it; one command at a time
//   READ   copies the output printed since the last READ; running = 0
//          once the command has finished
//   STATUS ticket_ms_left for the caller
//   DROP   ends the caller's ticket (root -k)
#define SYS_ROOT 39

#define SYS_ROOT_AUTH   1u
#define SYS_ROOT_RUN    2u
#define SYS_ROOT_READ   3u
#define SYS_ROOT_STATUS 4u
#define SYS_ROOT_DROP   5u
#define SYS_ROOT_TICKET_MS (5u * 60u * 1000u)
#define SYS_ROOT_OUTPUT_SIZE 2048u

typedef struct {
    uint32_t op;
    uint32_t running;           // READ: 1 while the command still runs
    char username[32];
    char password[64];
    char command[256];
    uint64_t ticket_ms_left;    // STATUS/AUTH/RUN
    uint32_t output_length;     // READ
    char output[SYS_ROOT_OUTPUT_SIZE];
} syscall_root_t;

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


// ===========================================
// STORAGE DEVICES (SYS_STORAGE)
// ===========================================
//
// rdi = syscall_storage_request_t*. Read-only view of the disks and
// partitions the kernel knows (the 'disks' command shows the same). Used
// by the MOSdesk Installer. Changing disks goes through SYS_ROOT commands
// (part, format, install), which need an account password.
#define SYS_STORAGE 41
#define SYS_STORAGE_ABI_VERSION 1u

#define SYS_STORAGE_LIST   1u   // fill devices[capacity], returns the count
#define SYS_STORAGE_STATUS 2u   // install_payload, boot_device

#define SYSCALL_STORAGE_KIND_DISK      0u
#define SYSCALL_STORAGE_KIND_PARTITION 1u

#define SYSCALL_STORAGE_TABLE_NONE 0u
#define SYSCALL_STORAGE_TABLE_MBR  1u
#define SYSCALL_STORAGE_TABLE_GPT  2u

// fs: 0 unknown, 1 empty, 2 MinimaFS, 3 FAT, 4 partitioned
#define SYSCALL_STORAGE_FLAG_BOOT  (1u << 0)   // holds the running system (0:)
#define SYSCALL_STORAGE_FLAG_IN_USE (1u << 1)  // it or a partition on it is mounted

typedef struct {
    char name[16];          // "sata0", "nvme0p2"
    char parent[16];        // partitions: their disk
    char model[41];
    char label[37];
    uint8_t kind;
    uint8_t table;          // disks
    uint8_t fs;
    uint8_t part_kind;      // partitions: 0 minimafs, 1 esp, 2 biosboot, 3 fat, 4 other
    int32_t mounted_drive;  // -1 if not mounted
    uint32_t flags;
    uint32_t sector_size;
    uint64_t size_bytes;
} syscall_storage_device_t;

typedef struct {
    uint32_t size;          // sizeof(syscall_storage_request_t)
    uint32_t version;       // SYS_STORAGE_ABI_VERSION
    uint32_t op;
    uint32_t capacity;
    syscall_storage_device_t* devices;
    uint32_t install_payload;   // STATUS: 1 when 'install' can run
    char boot_device[16];       // STATUS: device mounted as 0:
} syscall_storage_request_t;

#endif // SYSCALL_H
