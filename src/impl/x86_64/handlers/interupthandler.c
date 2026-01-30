#include "x86_64/interupthandler.h"
#include "x86_64/scheduler.h"

#define MAX_INTERRUPTS 32

struct InterruptEntry {
    bool* condition;
    interrupt_func_t func;
    bool is_once;
    size_t limit;
    size_t run_count;
    bool active;
};

static struct InterruptEntry entries[MAX_INTERRUPTS];

void triggerinterrupt(interrupt_func_t func) {
    if (func) func();
}

void assigninterrupt(bool* condition, interrupt_func_t func) {
    for (int i = 0; i < MAX_INTERRUPTS; ++i) {
        if (!entries[i].active) {
            entries[i] = (struct InterruptEntry){
                .condition = condition,
                .func = func,
                .is_once = false,
                .limit = 0,
                .run_count = 0,
                .active = true
            };
            return;
        }
    }
}

void triggerinterrupt_o(interrupt_func_t func, size_t limit) {
    static size_t counter = 0;
    if (counter < limit) {
        if (func) func();
        counter++;
    }
}

void assigninterrupt_o(bool* condition, interrupt_func_t func, size_t limit) {
    for (int i = 0; i < MAX_INTERRUPTS; ++i) {
        if (!entries[i].active) {
            entries[i] = (struct InterruptEntry){
                .condition = condition,
                .func = func,
                .is_once = true,
                .limit = limit,
                .run_count = 0,
                .active = true
            };
            return;
        }
    }
}

void reset_o() {
    for (int i = 0; i < MAX_INTERRUPTS; ++i) {
        if (entries[i].is_once) {
            entries[i].run_count = 0;
        }
    }
}

void interruptdispatcher_tick() {
    for (int i = 0; i < MAX_INTERRUPTS; ++i) {
        if (!entries[i].active || !entries[i].condition || !*(entries[i].condition)) continue;

        if (!entries[i].is_once || (entries[i].run_count < entries[i].limit)) {
            if (entries[i].func) entries[i].func();
            entries[i].run_count++;

            if (entries[i].is_once && entries[i].run_count >= entries[i].limit) {
                entries[i].active = false;
            }
        }
    }
}
