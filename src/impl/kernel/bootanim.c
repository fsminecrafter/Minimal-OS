#include "x86_64/bootanim.h"
#include "x86_64/gpu.h"
#include "x86_64/mmio.h"
#include "x86_64/sysconf.h"
#include "prochandler.h"
#include "graphics.h"
#include "serial.h"
#include "time.h"
#include "x86_64/clock.h"
#include "string.h"
#include "../../resources/bootlogo.h"     // Icon.png as coverage (tools/media/make_bootlogo.py)

/*
 * See bootanim.h. Three styles (BootupAnimStyle in System.conf, or bootanim=<style> on the kernel command
 * line, which also counts before the disk is mounted):
 *
 *   logo       the Minimal-OS logo (Icon.png) and five dots circling under it (the default)
 *   pulse      the logo breathing, with a light sweeping across it
 *   hourglass  the logo's hourglass mark turning over, again and again
 *
 * Every style starts with the logo fading in: System.conf can only be read once the disk is mounted, so
 * until then it shows what the logo styles have in common.
 *
 */

#define FRAME_MS   33u

#define COLOR_BG     0xFF000000u
#define COLOR_LINE   0xFFE8EEF3u

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

/* ------------------------------------------------------------ drawing */

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

// What the frames covered (cleared when the animation stops).
static void box_add(int x0, int y0, int x1, int y1) {
    if (g_box_x1 <= g_box_x0) { g_box_x0 = x0; g_box_y0 = y0; g_box_x1 = x1; g_box_y1 = y1; return; }
    if (x0 < g_box_x0) g_box_x0 = x0;
    if (y0 < g_box_y0) g_box_y0 = y0;
    if (x1 > g_box_x1) g_box_x1 = x1;
    if (y1 > g_box_y1) g_box_y1 = y1;
}

/* ------------------------------------------------------------ the logo */

enum { STYLE_LOGO, STYLE_PULSE, STYLE_HOURGLASS };
static volatile int g_style = STYLE_LOGO;
static volatile bool g_style_known;             // from the command line or System.conf
static int g_drawn_style = -1;                  // what the screen shows (a change clears it)
static int g_logo_level = -1;                   // brightness the static logo was last drawn at (256 = full)

#define INTRO_MS   650u                         // the logo fades in
#define PULSE_MS   2400u                        // one breath / one sweep
#define ORBIT_MS   2600u                        // one turn of the dots
#define COLOR_ACCENT 0xFF4FA3E0u

// Coverage of the logo at (u, v) in its own pixels (bilinear).
static int logo_cov(float u, float v) {
    if (u < 0.0f || v < 0.0f) return 0;
    int x = (int)u, y = (int)v;
    if (x >= (int)BOOTLOGO_SIZE - 1 || y >= (int)BOOTLOGO_SIZE - 1) return 0;
    float fx = u - (float)x, fy = v - (float)y;
    const uint8_t* p = &k_bootlogo[(size_t)y * BOOTLOGO_SIZE + (size_t)x];
    float a = p[0] + (p[1] - p[0]) * fx;
    float b = p[BOOTLOGO_SIZE] + (p[BOOTLOGO_SIZE + 1] - p[BOOTLOGO_SIZE]) * fx;
    return (int)(a + (b - a) * fy);
}

// Where the logo goes on a w x h screen: its top-left corner and side.
static void logo_place(int w, int h, int* lx, int* ly, int* side) {
    int m = w < h ? w : h;
    int s = m * 34 / 100;
    if (s < 96) s = 96;
    *side = s;
    *lx = (w - s) / 2;
    *ly = (h - s) / 2 - s / 10;
}

// The logo at brightness level (0..256). sweep >= 0: a light band at that x (0..1 across the logo).
static void draw_logo(const gpu_device_t* g, int level, float sweep) {
    int w = (int)g->width, h = (int)g->height, lx, ly, side;
    logo_place(w, h, &lx, &ly, &side);
    float k = (float)BOOTLOGO_SIZE / (float)side;
    int y_first = ly + (int)((float)BOOTLOGO_FIRST_ROW / k) - 1, y_last = ly + (int)((float)BOOTLOGO_LAST_ROW / k) + 2;
    if (y_first < 0) y_first = 0;
    if (y_last > h) y_last = h;
    int x0 = lx < 0 ? 0 : lx, x1 = lx + side > w ? w : lx + side;
    uint32_t bg = gpu_pack_color(g, COLOR_BG);
    for (int y = y_first; y < y_last; y++) {
        volatile uint32_t* row = g->fb + ((size_t)y * g->pitch) / 4u;
        float v = ((float)(y - ly) + 0.5f) * k - 0.5f;
        for (int x = x0; x < x1; x++) {
            float u = ((float)(x - lx) + 0.5f) * k - 0.5f;
            int c = logo_cov(u, v);
            if (!c) { row[x] = bg; continue; }
            uint32_t col = COLOR_LINE;
            if (sweep >= 0.0f) {
                float d = (float)(x - lx) / (float)side - sweep;
                if (d < 0) d = -d;
                if (d < 0.12f) col = blend(COLOR_LINE, COLOR_ACCENT, (int)((0.12f - d) / 0.12f * 200.0f));
            }
            row[x] = gpu_pack_color(g, blend(COLOR_BG, col, c * level / 255));
        }
    }
    box_add(x0, y_first, x1, y_last);
}

// Five dots circling under the logo, bunching up and spreading out (each a little behind the one before).
static void draw_dots(const gpu_device_t* g, uint32_t ms, int level) {
    int w = (int)g->width, h = (int)g->height, lx, ly, side;
    logo_place(w, h, &lx, &ly, &side);
    float r = (float)side * 0.075f, dot = (float)side * 0.012f + 1.2f;
    float cx = (float)w * 0.5f, cy = (float)(ly + side) + r * 2.2f;
    int bx0 = (int)(cx - r - dot - 2), by0 = (int)(cy - r - dot - 2), bx1 = (int)(cx + r + dot + 3), by1 = (int)(cy + r + dot + 3);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > w) bx1 = w;
    if (by1 > h) by1 = h;
    float px[5], py[5];
    for (int i = 0; i < 5; i++) {
        float t = (float)((ms + ORBIT_MS - (uint32_t)i * 150u) % ORBIT_MS) / (float)ORBIT_MS;
        // Slow at the top, fast at the bottom: the angle eases in and out over a turn.
        float e = t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
        float s, c;
        sincos_deg(e * 360.0f, &s, &c);
        px[i] = cx + s * r;
        py[i] = cy - c * r;
    }
    uint32_t bg = gpu_pack_color(g, COLOR_BG);
    for (int y = by0; y < by1; y++) {
        volatile uint32_t* row = g->fb + ((size_t)y * g->pitch) / 4u;
        float fy = (float)y + 0.5f;
        for (int x = bx0; x < bx1; x++) {
            float fx = (float)x + 0.5f, best = 1e9f;
            for (int i = 0; i < 5; i++) {
                float dx = fx - px[i], dy = fy - py[i], d2 = dx * dx + dy * dy, d;
                if (d2 > (dot + 2) * (dot + 2)) continue;
                __asm__("sqrtss %1, %0" : "=x"(d) : "x"(d2));
                if (d - dot < best) best = d - dot;
            }
            if (best >= 0.5f) { row[x] = bg; continue; }
            float cov = best <= -0.5f ? 1.0f : 0.5f - best;
            row[x] = gpu_pack_color(g, blend(COLOR_BG, COLOR_LINE, (int)(cov * (float)level)));
        }
    }
    box_add(bx0, by0, bx1, by1);
}

// The mark of the logo (its rows 16..254: the hourglass without the name) turning over: it rests, then
// turns half a circle, like an hourglass being flipped.
#define MARK_TOP    16.0f
#define MARK_BOTTOM 254.0f
#define MARK_CX     159.5f
#define MARK_CY     135.0f
#define MARK_R      170.0f                      // centre to the farthest corner, in logo pixels
#define FLIP_REST_MS 1000u
#define FLIP_TURN_MS 850u
static void draw_mark_turning(const gpu_device_t* g, uint32_t ms, int level) {
    int w = (int)g->width, h = (int)g->height;
    int m = w < h ? w : h;
    float side = (float)(m * 30 / 100);         // the mark as big as the logo styles' whole logo
    float k = (float)BOOTLOGO_SIZE / side;      // logo pixels per screen pixel
    uint32_t cyc = FLIP_REST_MS + FLIP_TURN_MS, n = ms / cyc, t = ms % cyc;
    float deg = 180.0f * (float)(n & 1u);
    if (t > FLIP_REST_MS) deg += 180.0f * smooth((float)(t - FLIP_REST_MS) / (float)FLIP_TURN_MS);
    float s, c;
    sincos_deg(deg, &s, &c);
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    int reach = (int)(MARK_R / k) + 2;
    int x0 = (int)cx - reach, y0 = (int)cy - reach, x1 = (int)cx + reach, y1 = (int)cy + reach;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    uint32_t bg = gpu_pack_color(g, COLOR_BG);
    for (int y = y0; y < y1; y++) {
        volatile uint32_t* row = g->fb + ((size_t)y * g->pitch) / 4u;
        float dy = ((float)y + 0.5f - cy) * k;
        for (int x = x0; x < x1; x++) {
            float dx = ((float)x + 0.5f - cx) * k;
            // Back into the logo: rotate by -deg around the mark's centre.
            float u = MARK_CX + dx * c + dy * s, v = MARK_CY - dx * s + dy * c;
            int cov = (v >= MARK_TOP - 1.0f && v <= MARK_BOTTOM + 1.0f) ? logo_cov(u, v) : 0;
            row[x] = cov ? gpu_pack_color(g, blend(COLOR_BG, COLOR_LINE, cov * level / 255)) : bg;
        }
    }
    box_add(x0, y0, x1, y1);
}

static void draw_frame(uint32_t ms) {
    const gpu_device_t* g = target();
    if (!g) return;
    int w = (int)g->width, h = (int)g->height;
    // Until the setting is known, the logo style (every style starts with the logo).
    int style = g_style_known ? g_style : STYLE_LOGO;
    if (g->fb != g_last_fb || (uint32_t)w != g_last_w || (uint32_t)h != g_last_h || style != g_drawn_style) {
        // A new screen (the driver took over, the mode changed) or another style: start black.
        fill_rect(g, 0, 0, w, h, COLOR_BG);
        g_last_fb = g->fb;
        g_last_w = (uint32_t)w;
        g_last_h = (uint32_t)h;
        g_box_x1 = g_box_x0 = 0;
        g_drawn_style = style;
        g_logo_level = -1;
    }
    int level = ms >= INTRO_MS ? 256 : (int)(smooth((float)ms / (float)INTRO_MS) * 256.0f);
    switch (style) {
        case STYLE_HOURGLASS:
            draw_mark_turning(g, ms, level);
            break;
        case STYLE_PULSE: {
            // Breathes between 60 % and full brightness; a light crosses it once per breath.
            float s, c;
            sincos_deg((float)(ms % PULSE_MS) / (float)PULSE_MS * 360.0f, &s, &c);
            int breath = (int)(0.80f * 256.0f + 0.20f * 256.0f * c);
            float sweep = (float)(ms % PULSE_MS) / (float)PULSE_MS * 1.6f - 0.3f;
            draw_logo(g, ms < INTRO_MS ? level : breath, ms < INTRO_MS ? -1.0f : sweep);
            break;
        }
        default:
            // The logo is drawn again only while it fades in; the dots every frame.
            if (level != g_logo_level) { draw_logo(g, level, -1.0f); g_logo_level = level; }
            draw_dots(g, ms, level);
            break;
    }
}

/* ------------------------------------------------------------ control */

void bootanim_start(multiboot2_info_t* mb_info) {
    if (!mb_info || multiboot2_has_cmdline_token(mb_info, "nobootanim") ||
        multiboot2_has_cmdline_token(mb_info, "--debug"))
        return;
    multiboot2_framebuffer_info_t info;
    static const char* const k_styles[3] = { "bootanim=logo", "bootanim=pulse", "bootanim=hourglass" };
    for (int i = 0; i < 3; i++)
        if (multiboot2_has_cmdline_token(mb_info, k_styles[i])) { g_style = i; g_style_known = true; }
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
        return;
    }
    // The style, unless the command line chose one.
    if (!g_style_known && sysconf_get("BootupAnimStyle", v, sizeof(v))) {
        int style = (v[0] == 'p' || v[0] == 'P') ? STYLE_PULSE : (v[0] == 'h' || v[0] == 'H') ? STYLE_HOURGLASS : STYLE_LOGO;
        g_style = style;
        serial_write_str("bootanim: style from System.conf: ");
        serial_write_str(v);
        serial_write_str("\n");
    }
    g_style_known = true;
}
