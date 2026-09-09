#include "x86_64/thread.h"
#include "x86_64/scheduler.h"
#include "x86_64/proc.h"
#include "prochandler.h"
#include "serial.h"

thread_id_t thread_create(const char* name, void (*entry)(void)) {
    if (!entry) return 0;
    process_t* t = createProcess(name ? name : "thread", entry);
    return t ? t->pid : 0;
}

void thread_join(thread_id_t tid) {
    if (tid == 0) return;

    for (;;) {
        process_t* t = findProcessByPID(tid);
        if (!t) return;  // already reaped by the scheduler's cleanup pass
        if (t->state == PROCESS_ZOMBIE || t->state == PROCESS_TERMINATED) return;

        // Poll rather than busy-spin so we actually yield the CPU to
        // the thread we're waiting on.
        sleep(5);
    }
}

void thread_exit(void) {
    process_exit();
}

thread_id_t thread_self(void) {
    return getCurrentPID();
}