#include "mouse/usbmouse.h"
#include "serial.h"
#include "string.h"

static usb_mouse_state_t g_mouse_state = {0};
static bool g_mouse_seen = false;

void usb_mouse_init(void) {
    serial_write_str("USB Mouse: Initializing...\n");
    memset(&g_mouse_state, 0, sizeof(g_mouse_state));
    g_mouse_seen = false;
    serial_write_str("USB Mouse: Ready (waiting for device)\n");
}

void usb_mouse_process_report(const usb_hid_mouse_report_t* report) {
    if (!report) return;

    g_mouse_seen = true;
    g_mouse_state.current_report = *report;

    g_mouse_state.left   = (report->buttons & USB_MOUSE_BTN_LEFT)   != 0;
    g_mouse_state.right  = (report->buttons & USB_MOUSE_BTN_RIGHT)  != 0;
    g_mouse_state.middle = (report->buttons & USB_MOUSE_BTN_MIDDLE) != 0;

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
