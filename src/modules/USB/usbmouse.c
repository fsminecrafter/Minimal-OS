#include "mouse/usbmouse.h"
#include "input.h"
#include "serial.h"
#include "string.h"

static usb_mouse_state_t g_mouse_state = {0};
static bool g_mouse_seen = false;
static uint8_t g_last_event_buttons;

void usb_mouse_init(void) {
    serial_write_str("USB Mouse: Initializing...\n");
    memset(&g_mouse_state, 0, sizeof(g_mouse_state));
    g_mouse_seen = false;
    g_last_event_buttons = 0;
    serial_write_str("USB Mouse: Ready (waiting for device)\n");
}

void usb_mouse_process_motion(uint8_t raw_buttons, int32_t dx, int32_t dy, int32_t wheel) {
    g_mouse_seen = true;
    g_mouse_state.left   = (raw_buttons & USB_MOUSE_BTN_LEFT)   != 0;
    g_mouse_state.right  = (raw_buttons & USB_MOUSE_BTN_RIGHT)  != 0;
    g_mouse_state.middle = (raw_buttons & USB_MOUSE_BTN_MIDDLE) != 0;
    uint8_t buttons = raw_buttons &
        (USB_MOUSE_BTN_LEFT | USB_MOUSE_BTN_RIGHT | USB_MOUSE_BTN_MIDDLE);
    if (dx || dy)
        input_event_push(SYSCALL_INPUT_MOUSE_MOVE, 0, 0, buttons, dx, dy, 0);
    if (buttons != g_last_event_buttons) {
        input_event_push(SYSCALL_INPUT_MOUSE_BUTTON, 0, 0, buttons, 0, 0, 0);
        g_last_event_buttons = buttons;
    }
    if (wheel)
        input_event_push(SYSCALL_INPUT_MOUSE_WHEEL, 0, 0, buttons, 0, 0, wheel);
    g_mouse_state.accum_dx    += dx;
    g_mouse_state.accum_dy    += dy;
    g_mouse_state.accum_wheel += wheel;
}

void usb_mouse_process_report(const usb_hid_mouse_report_t* report) {
    if (!report) return;

    g_mouse_seen = true;
    g_mouse_state.current_report = *report;

    g_mouse_state.left   = (report->buttons & USB_MOUSE_BTN_LEFT)   != 0;
    g_mouse_state.right  = (report->buttons & USB_MOUSE_BTN_RIGHT)  != 0;
    g_mouse_state.middle = (report->buttons & USB_MOUSE_BTN_MIDDLE) != 0;

    uint8_t buttons = report->buttons &
        (USB_MOUSE_BTN_LEFT | USB_MOUSE_BTN_RIGHT | USB_MOUSE_BTN_MIDDLE);
    if (report->x || report->y) {
        input_event_push(SYSCALL_INPUT_MOUSE_MOVE, 0, 0, buttons,
                         report->x, report->y, 0);
    }
    if (buttons != g_last_event_buttons) {
        input_event_push(SYSCALL_INPUT_MOUSE_BUTTON, 0, 0, buttons,
                         0, 0, 0);
        g_last_event_buttons = buttons;
    }
    if (report->wheel) {
        input_event_push(SYSCALL_INPUT_MOUSE_WHEEL, 0, 0, buttons,
                         0, 0, report->wheel);
    }

    g_mouse_state.accum_dx    += report->x;
    g_mouse_state.accum_dy    += report->y;
    g_mouse_state.accum_wheel += report->wheel;
}

const usb_mouse_state_t* usb_mouse_get_state(void) {
    return &g_mouse_state;
}

void usb_mouse_consume_delta(int32_t* out_dx, int32_t* out_dy,
                             int32_t* out_wheel, uint8_t* out_buttons) {
    if (out_dx)    *out_dx    = g_mouse_state.accum_dx;
    if (out_dy)    *out_dy    = g_mouse_state.accum_dy;
    if (out_wheel) *out_wheel = g_mouse_state.accum_wheel;
    if (out_buttons) {
        *out_buttons =
            (g_mouse_state.left   ? USB_MOUSE_BTN_LEFT   : 0) |
            (g_mouse_state.right  ? USB_MOUSE_BTN_RIGHT  : 0) |
            (g_mouse_state.middle ? USB_MOUSE_BTN_MIDDLE : 0);
    }

    g_mouse_state.accum_dx    = 0;
    g_mouse_state.accum_dy    = 0;
    g_mouse_state.accum_wheel = 0;
}

bool usb_mouse_is_present(void) {
    return g_mouse_seen;
}
