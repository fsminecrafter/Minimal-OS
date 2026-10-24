/*
 * Window manager watchdog.
 *
 * mosWM draws the mouse pointer and calls into the kernel every frame
 * (input, uptime, sleep). If it makes no syscall for 10 s, it is stuck:
 * the screen and pointer freeze although the kernel is still running, so
 * the cpu0 hang watchdog in lapic_timer.c never fires. This check runs on
 * cpu0's PIT tick and panics, so the panic screen shows where mosWM is.
 */
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/wm_watchdog.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "panic.h"
#include "time.h"

#define WM_STALL_MS 10000u

static process_t* volatile g_wm_proc;
static volatile uint64_t g_wm_last_ms;
static volatile uint64_t g_wm_syscall;
static volatile uint64_t g_wm_syscall_start_ms;
static volatile bool g_wm_in_syscall;

void wm_watchdog_syscall_enter(uint64_t num) {
    process_t* p = current_process;
    if (!p || !p->is_window_manager) return;
    uint64_t now = time_get_uptime_ms();
    g_wm_proc = p;
    g_wm_syscall = num;
    g_wm_syscall_start_ms = now;
    g_wm_in_syscall = true;
    g_wm_last_ms = now;
}

void wm_watchdog_syscall_exit(void) {
    process_t* p = current_process;
    if (!p || p != g_wm_proc) return;
    g_wm_in_syscall = false;
    g_wm_last_ms = time_get_uptime_ms();
}

void wm_watchdog_forget(const process_t* proc) {
    if (proc && proc == g_wm_proc) g_wm_proc = NULL;
}

static void append(char* buf, int* n, const char* s) {
    while (*s && *n < 150) buf[(*n)++] = *s++;
    buf[*n] = 0;
}

static void append_dec(char* buf, int* n, uint64_t v) {
    char t[21];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && *n < 150) buf[(*n)++] = t[--k];
    buf[*n] = 0;
}

void wm_watchdog_tick(void) {
    static bool fired;
    process_t* p = g_wm_proc;
    if (fired || !p || panic_in_progress()) return;
    if (p->state == PROCESS_ZOMBIE || p->state == PROCESS_TERMINATED ||
        p->state == PROCESS_PAUSED) return;
    uint64_t now = time_get_uptime_ms();
    if (now - g_wm_last_ms < WM_STALL_MS) return;
    fired = true;

    static const char* const states[] = { "ready", "running", "waiting", "paused", "zombie", "terminated" };
    static char message[160];
    int n = 0;
    append(message, &n, "Watchdog: mosWM (pid ");
    append_dec(message, &n, p->pid);
    append(message, &n, ") stuck 10 s, ");
    append(message, &n, (uint32_t)p->state < 6 ? states[p->state] : "?");
    append(message, &n, " cpu");
    append_dec(message, &n, p->sched_cpu);
    if (g_wm_in_syscall) {
        append(message, &n, ", in syscall ");
        append_dec(message, &n, g_wm_syscall);
        append(message, &n, " for ");
        append_dec(message, &n, (now - g_wm_syscall_start_ms) / 1000);
        append(message, &n, " s");
    } else {
        append(message, &n, ", in its own code (last syscall ");
        append_dec(message, &n, g_wm_syscall);
        append(message, &n, ")");
    }
    panic_set_focus_process(p);
    PANIC(message);
}
