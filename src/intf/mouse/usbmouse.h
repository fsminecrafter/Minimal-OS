#ifndef USB_MOUSE_H
#define USB_MOUSE_H

#include <stdint.h>
#include <stdbool.h>

// Standard USB HID boot mouse report (3-4 bytes):
//   byte 0: button bitmask (bit0=left, bit1=right, bit2=middle)
//   byte 1: X movement (signed, relative)
//   byte 2: Y movement (signed, relative)
//   byte 3: wheel movement (signed, relative) - not sent by every mouse;
//           the report buffer is always zeroed before each transfer, so
//           this reads 0 on 3-byte devices instead of stale data.
typedef struct {
    uint8_t buttons;
    int8_t  x;
    int8_t  y;
    int8_t  wheel;
} __attribute__((packed)) usb_hid_mouse_report_t;

#define USB_MOUSE_BTN_LEFT   (1 << 0)
#define USB_MOUSE_BTN_RIGHT  (1 << 1)
#define USB_MOUSE_BTN_MIDDLE (1 << 2)

typedef struct {
    bool left;
    bool right;
    bool middle;

    usb_hid_mouse_report_t current_report;

    // Movement/wheel accumulated since the last usb_mouse_consume_delta()
    // call - "how far did it move", not an absolute screen position.
    int32_t accum_dx;
    int32_t accum_dy;
    int32_t accum_wheel;
} usb_mouse_state_t;

// Resets driver state. Safe to call with no USB mouse present.
void usb_mouse_init(void);

// Feeds one HID boot mouse report into the driver. Called by the UHCI
// driver's interrupt-poll path (see uhci.h).
void usb_mouse_process_report(const usb_hid_mouse_report_t* report);

// Read-only snapshot: current buttons + last raw report.
const usb_mouse_state_t* usb_mouse_get_state(void);

// Reads AND RESETS the accumulated dx/dy/wheel since the last call, and
// reports current button state. Primary API for anything asking "how
// much has the mouse moved since I last checked" - the SYS_MOUSE
// syscall (syscall.c) goes through this.
void usb_mouse_consume_delta(int32_t* out_dx, int32_t* out_dy,
                             int32_t* out_wheel, uint8_t* out_buttons);

bool usb_mouse_is_present(void);

#endif // USB_MOUSE_H
