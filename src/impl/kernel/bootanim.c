#include "x86_64/bootanim.h"
#include "x86_64/gpu.h"
#include "x86_64/mmio.h"
#include "x86_64/sysconf.h"
#include "prochandler.h"
#include "graphics.h"
#include "serial.h"
#include "time.h"
#include "x86_64/clock.h"

/*
 * See bootanim.h. The glass is two halves joined at the waist; each half is
 * a bar and two sides drawn as anti-aliased capsules, with a faint fill.
 * A half points along an angle measured counter-clockwise from "up"
 * (0 up, 90 left, 180 down, -90 right). The four phases of a cycle:
 *
 *   A  top 0 -> 90             the top half falls to the left
 *   B  top 90 -> 180,          the whole glass turns on: the fallen half
 *      bottom 180 -> 360       ends at the bottom, the other on top
 *   C  bottom 0 -> -90         the new top half falls to the right
 *   D  bottom -90 -> -180,     it turns back the other way to the start
 *      top 180 -> 0
 */

#define FRAME_MS   33u
#define FALL_MS    700u
#define TURN_MS    900u
#define HOLD_MS    260u
#define CYCLE_MS   (2u * (FALL_MS + HOLD_MS) + 2u * (TURN_MS + HOLD_MS))

#define COLOR_BG     0xFF000000u
#define COLOR_LINE   0xFFE8EEF3u
#define COLOR_FILL   0xFF4FA3E0u

static volatile bool g_active;          // drawing frames
static volatile bool g_hiding;          // the console only keeps its buffer
static volatile bool g_paused;
static volatile int g_drawing;          // a frame is being drawn now
static uint64_t g_ticks;                // ms since start (one PIT tick each)
static uint64_t g_next_frame;
static gpu_device_t g_early;            // the GRUB framebuffer, mapped here
static const volatile uint32_t* g_last_fb;
static uint32_t g_last_w, g_last_h;
static int g_box_x0, g_box_y0, g_box_x1, g_box_y1;  // what the last frame covered
static uint64_t g_deadline;             // ms; 0 = none
static uint32_t g_frames;               // drawn so far (serial log)
static uint64_t g_start_ns;             // the TSC clock when it started
static uint64_t now_ms(void) { return (clock_monotonic_ns() - g_start_ns) / 1000000u; }

bool bootanim_running(void) { return g_active; }
bool bootanim_hides_console(void) { return g_hiding; }
void bootanim_pause(bool paused) { g_paused = paused; }

/* ------------------------------------------------------------ math */

static float fabs_(float x) { return x < 0 ? -x : x; }

// sin and cos of an angle in degrees (a short polynomial; good enough for pixels).
static void sincos_deg(float deg, float* s, float* c) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    float x = deg * 0.017453292f;
    // sin on [-pi, pi]: reduce to [-pi/2, pi/2] first.
    float xs = x;
    if (xs > 1.5707963f) xs = 3.1415927f - xs;
    if (xs < -1.5707963f) xs = -3.1415927f - xs;
    float x2 = xs * xs;
    *s = xs * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
    // cos = sin(x + 90 degrees)
    float y = x + 1.5707963f;
    if (y > 3.1415927f) y -= 6.2831853f;
    if (y > 1.5707963f) y = 3.1415927f - y;
    if (y < -1.5707963f) y = -3.1415927f - y;
    float y2 = y * y;
    *c = y * (1.0f - y2 / 6.0f * (1.0f - y2 / 20.0f * (1.0f - y2 / 42.0f * (1.0f - y2 / 72.0f))));
}

static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// A fall: faster and faster, then a small bounce where it lands.
static float fall(float t) {
    if (t < 0.78f) { float u = t / 0.78f; return u * u; }
    float u = (t - 0.78f) / 0.22f;          // 0..1
    float s, c;
    sincos_deg(u * 180.0f, &s, &c);
    return 1.0f - 0.07f * s;
}

// The two halves' angles at `ms` into the cycle.
static void pose(uint32_t ms, float* top, float* bottom) {
    uint32_t t = ms % CYCLE_MS;
    if (t < FALL_MS) { *top = 90.0f * fall((float)t / FALL_MS); *bottom = 180.0f; return; }
    t -= FALL_MS;
    if (t < HOLD_MS) { *top = 90.0f; *bottom = 180.0f; return; }
    t -= HOLD_MS;
    if (t < TURN_MS) {
        float e = smooth((float)t / TURN_MS);
        *top = 90.0f + 90.0f * e;
        *bottom = 180.0f + 180.0f * e;
        return;
    }
    t -= TURN_MS;
    if (t < HOLD_MS) { *top = 180.0f; *bottom = 0.0f; return; }
    t -= HOLD_MS;
    if (t < FALL_MS) { *top = 180.0f; *bottom = -90.0f * fall((float)t / FALL_MS); return; }
    t -= FALL_MS;
    if (t < HOLD_MS) { *top = 180.0f; *bottom = -90.0f; return; }
    t -= HOLD_MS;
    if (t < TURN_MS) {
        float e = smooth((float)t / TURN_MS);
        *bottom = -90.0f - 90.0f * e;
        *top = 180.0f - 180.0f * e;
        return;
    }
    *top = 0.0f;
    *bottom = 180.0f;
}

/* ------------------------------------------------------------ drawing */

typedef struct { float ax, ay, bx, by, r; } capsule_t;
typedef struct { float x[3], y[3]; } tri_t;

typedef struct {
    capsule_t cap[6];
    tri_t tri[2];
    int x0, y0, x1, y1;                 // bounding box
} shape_t;

static void half(shape_t* sh, int k, float cx, float cy, float deg, float len, float wid, float line, float bar) {
    float s, c;
    sincos_deg(deg, &s, &c);
    float ax = -s, ay = -c;             // outward, screen y down
    float px = -ay, py = ax;            // across
    float ex = cx + ax * len, ey = cy + ay * len;
    float lx = ex + px * wid, ly = ey + py * wid, rx = ex - px * wid, ry = ey - py * wid;
    float bw = wid * 1.18f;
    sh->cap[k * 3 + 0] = (capsule_t){ cx, cy, lx, ly, line };
    sh->cap[k * 3 + 1] = (capsule_t){ cx, cy, rx, ry, line };
    sh->cap[k * 3 + 2] = (capsule_t){ ex + px * bw, ey + py * bw, ex - px * bw, ey - py * bw, bar };
    sh->tri[k] = (tri_t){ { cx, lx, rx }, { cy, ly, ry } };
}

static float capsule_dist(const capsule_t* c, float x, float y) {
    float dx = c->bx - c->ax, dy = c->by - c->ay;
    float qx = x - c->ax, qy = y - c->ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? (qx * dx + qy * dy) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ox = qx - dx * t, oy = qy - dy * t;
    float d2 = ox * ox + oy * oy;
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(d2));   // the kernel has no libm
    return r - c->r;
}

static bool in_tri(const tri_t* t, float x, float y) {
    float d1 = (x - t->x[1]) * (t->y[0] - t->y[1]) - (t->x[0] - t->x[1]) * (y - t->y[1]);
    float d2 = (x - t->x[2]) * (t->y[1] - t->y[2]) - (t->x[1] - t->x[2]) * (y - t->y[2]);
    float d3 = (x - t->x[0]) * (t->y[2] - t->y[0]) - (t->x[2] - t->x[0]) * (y - t->y[0]);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static uint32_t blend(uint32_t a, uint32_t b, int t) {        // t of 256 toward b
    uint32_t r = (((a >> 16) & 255) * (uint32_t)(256 - t) + ((b >> 16) & 255) * (uint32_t)t) >> 8;
    uint32_t g = (((a >> 8) & 255) * (uint32_t)(256 - t) + ((b >> 8) & 255) * (uint32_t)t) >> 8;
    uint32_t bl = ((a & 255) * (uint32_t)(256 - t) + (b & 255) * (uint32_t)t) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static const gpu_device_t* target(void) {
    gpu_device_t* sys = getSystemGPU();
    if (sys && sys->fb && sys->width && sys->height) return sys;
    if (g_early.fb) return &g_early;
    return 0;
}

static void fill_rect(const gpu_device_t* g, int x0, int y0, int x1, int y1, uint32_t argb) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g->width) x1 = (int)g->width;
    if (y1 > (int)g->height) y1 = (int)g->height;
    uint32_t px = gpu_pack_color(g, argb);
    for (int y = y0; y < y1; y++) {
        volatile uint32_t* row = g->fb + ((size_t)y * g->pitch) / 4u;
        for (int x = x0; x < x1; x++) row[x] = px;
    }
}

static void draw_frame(uint32_t ms) {
    const gpu_device_t* g = target();
    if (!g) return;
    int w = (int)g->width, h = (int)g->height;
    if (g->fb != g_last_fb || (uint32_t)w != g_last_w || (uint32_t)h != g_last_h) {
        // A new screen (the driver took over, the mode changed): start black.
        fill_rect(g, 0, 0, w, h, COLOR_BG);
        g_last_fb = g->fb;
        g_last_w = (uint32_t)w;
        g_last_h = (uint32_t)h;
        g_box_x1 = g_box_x0 = 0;
    }
    float size = (float)(w < h ? w : h);
    float len = size * 0.13f, wid = len * 0.62f, line = size * 0.0075f + 1.0f, bar = line * 1.35f;
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    float top, bottom;
    pose(ms, &top, &bottom);
    shape_t sh;
    half(&sh, 0, cx, cy, top, len, wid, line, bar);
    half(&sh, 1, cx, cy, bottom, len, wid, line, bar);
    // Everything the glass can reach in any pose: a square around the waist.
    int reach = (int)(len * 1.32f + bar + 3.0f);
    int x0 = (int)cx - reach, y0 = (int)cy - reach, x1 = (int)cx + reach, y1 = (int)cy + reach;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    uint32_t bg = gpu_pack_color(g, COLOR_BG);
    uint32_t fill = blend(COLOR_BG, COLOR_FILL, 72);
    for (int y = y0; y < y1; y++) {
        volatile uint32_t* row = g->fb + ((size_t)y * g->pitch) / 4u;
        float fy = (float)y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float fx = (float)x + 0.5f;
            uint32_t base = (in_tri(&sh.tri[0], fx, fy) || in_tri(&sh.tri[1], fx, fy)) ? fill : COLOR_BG;
            float d = 1e9f;
            for (int k = 0; k < 6; k++) {
                const capsule_t* c = &sh.cap[k];
                // Cheap reject: outside the capsule's box plus a pixel.
                float lo_x = (c->ax < c->bx ? c->ax : c->bx) - c->r - 1.0f, hi_x = (c->ax > c->bx ? c->ax : c->bx) + c->r + 1.0f;
                float lo_y = (c->ay < c->by ? c->ay : c->by) - c->r - 1.0f, hi_y = (c->ay > c->by ? c->ay : c->by) + c->r + 1.0f;
                if (fx < lo_x || fx > hi_x || fy < lo_y || fy > hi_y) continue;
                float dk = capsule_dist(c, fx, fy);
                if (dk < d) d = dk;
            }
            uint32_t argb = base;
            if (d < 0.5f) {
                float cov = d <= -0.5f ? 1.0f : 0.5f - d;     // one pixel of anti-aliasing
                argb = blend(base, COLOR_LINE, (int)(cov * 256.0f));
            }
            row[x] = argb == COLOR_BG ? bg : gpu_pack_color(g, argb);
        }
    }
    g_box_x0 = x0; g_box_y0 = y0; g_box_x1 = x1; g_box_y1 = y1;
    (void)fabs_;
}

/* ------------------------------------------------------------ control */

void bootanim_start(multiboot2_info_t* mb_info) {
    if (!mb_info || multiboot2_has_cmdline_token(mb_info, "nobootanim") ||
        multiboot2_has_cmdline_token(mb_info, "--debug"))
        return;
    multiboot2_framebuffer_info_t info;
    if (!multiboot2_get_framebuffer(mb_info, &info) || info.framebuffer_type != 1 || info.bpp != 32 ||
        !info.width || !info.height || info.width > 8192 || info.height > 8192 || info.pitch < info.width * 4u)
        return;
    uint64_t bytes = (uint64_t)info.pitch * info.height;
    uintptr_t phys = (uintptr_t)info.address & ~0xFFFull;
    size_t off = (size_t)(info.address - phys);
    size_t map = (off + (size_t)bytes + 0xFFF) & ~(size_t)0xFFF;
    void* va = mmio_alloc_va(map);
    if (!va || !map_mmio_page_wc((uintptr_t)va, phys, map)) {
        serial_write_str("bootanim: cannot map the framebuffer\n");
        return;
    }
    g_early = (gpu_device_t){
        .fb = (volatile uint32_t*)((uintptr_t)va + off),
        .width = info.width, .height = info.height, .pitch = info.pitch, .bpp = 4,
        .red_position = info.red_position, .red_mask_size = info.red_mask_size,
        .green_position = info.green_position, .green_mask_size = info.green_mask_size,
        .blue_position = info.blue_position, .blue_mask_size = info.blue_mask_size,
        .fb_phys = info.address,
    };
    serial_write_str("bootanim: framebuffer ");
    serial_write_dec(info.width); serial_write_str("x"); serial_write_dec(info.height);
    serial_write_str(" pitch "); serial_write_dec(info.pitch);
    serial_write_str(" at 0x"); serial_write_hex(info.address);
    serial_write_str("\n");
    g_ticks = 0;
    g_next_frame = 0;
    g_start_ns = clock_monotonic_ns();
    g_hiding = true;
    __atomic_store_n(&g_active, true, __ATOMIC_RELEASE);
    serial_write_str("bootanim: started\n");
}

void bootanim_stop_after(uint32_t ms) { g_deadline = now_ms() + ms; }

void bootanim_abort(void) {
    g_active = false;
    g_hiding = false;
}


// Draws a frame when one is due. From the timer and from the serial log
// path (much of the boot runs with interrupts off, where no tick comes):
// whoever gets there first draws, the others return at once.
void bootanim_poll(void) {
    if (!g_active || g_paused) return;
    uint64_t t = now_ms();
    if (t < g_next_frame) return;
    if (__atomic_exchange_n(&g_drawing, 1, __ATOMIC_ACQ_REL)) return;
    if (g_active && !g_paused && t >= g_next_frame) {
        g_ticks = t;
        draw_frame((uint32_t)t);
        g_frames++;
        // At most about a quarter of the CPU: a slow frame (an emulator, a
        // big screen) spaces the next ones out, so the boot is not slowed.
        uint64_t spent = now_ms() - t;
        uint64_t gap = spent * 3u > FRAME_MS ? spent * 3u : FRAME_MS;
        g_next_frame = now_ms() + gap;
    }
    __atomic_store_n(&g_drawing, 0, __ATOMIC_RELEASE);
}

void bootanim_tick(void) {
    if (!g_active) return;
    if (g_deadline && now_ms() >= g_deadline) {
        // Nothing took the screen (the desktop did not start): give the
        // console back.
        g_active = false;
        const gpu_device_t* g = target();
        if (g && g_box_x1 > g_box_x0) fill_rect(g, g_box_x0, g_box_y0, g_box_x1, g_box_y1, COLOR_BG);
        g_hiding = false;
        if (graphics_get_width()) graphics_terminal_redraw();
        serial_write_str("bootanim: nothing took the screen; stopped\n");
        return;
    }
    bootanim_poll();
}

void bootanim_stop(bool redraw_console) {
    if (!__atomic_exchange_n(&g_active, false, __ATOMIC_ACQ_REL)) {
        g_hiding = false;
        return;
    }
    // A frame on the timer CPU finishes first.
    for (uint32_t spin = 0; __atomic_load_n(&g_drawing, __ATOMIC_ACQUIRE) && spin < 100000000u; spin++)
        __asm__ volatile("pause");
    const gpu_device_t* g = target();
    if (g && g_box_x1 > g_box_x0) fill_rect(g, g_box_x0, g_box_y0, g_box_x1, g_box_y1, COLOR_BG);
    g_hiding = false;
    if (redraw_console && graphics_get_width()) graphics_terminal_redraw();
    serial_write_str("bootanim: stopped after ");
    serial_write_dec(g_frames);
    serial_write_str(" frames, ");
    serial_write_dec(g_ticks);
    serial_write_str(" ms of ticks, uptime ");
    serial_write_dec(time_get_uptime_ms());
    serial_write_str(" ms, tsc clock ");
    serial_write_dec(clock_monotonic_ns() / 1000000u);
    serial_write_str(" ms, paused ");
    serial_write_dec(g_paused);
    serial_write_str("\n");
}

void bootanim_apply_setting(void) {
    if (!g_active) return;
    char v[16];
    if (sysconf_get("BootupAnim", v, sizeof(v)) && (v[0] == 'f' || v[0] == 'F' || v[0] == '0' || v[0] == 'n' || v[0] == 'N')) {
        serial_write_str("bootanim: off in System.conf\n");
        bootanim_stop(true);
    }
}
