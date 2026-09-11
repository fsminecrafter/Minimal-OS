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

#define SYS_O_RDONLY 0

#define SYS_SUCCESS       0
#define SYS_ERR_GENERIC  ((uint64_t)-1)
#define SYS_ERR_BADFD    ((uint64_t)-2)
#define SYS_ERR_NOTFOUND ((uint64_t)-3)
#define SYS_ERR_INVAL    ((uint64_t)-4)

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

// Called by isr_syscall_wrapped (syscall_isr.asm) for every `int 0x80`.
// regs->rax = syscall number in, return value out. Args in
// rdi/rsi/rdx (SysV-ish - matches a future `syscall`-instruction ABI
// too, if ring3 ever lands). Return values are in rax; failures are
// negative values represented as uint64_t.
void syscall_dispatch(syscall_regs_t* regs);

#endif // SYSCALL_H