#include <stdint.h>
#include "x86_64/proc.h"
#include "print.h"
#include "panic.h"

void test_process() {
    volatile int counter = 0;
    PANIC("Hello");
    while (1) {
        counter++;
        print_int(counter);
    }
}

void test_process2() {
    volatile int counter = 0;
    while (1) {
        counter++;
        print_int(counter);
        print_str("Yes hello");
    }
}

#include "x86_64/scheduler.h"  // to get current_process visible

void proc_test() {
    process_t* proc = proc_create("proc_example.c", test_process);
    if (!proc) {
        PANIC("Cannot start Proc: proc_example.c test_process");
        return;
    }

    current_process = proc;
    current_process->state = PROCESS_RUNNING;

    process_t* proc2 = proc_create("proc_example.c", test_process2);

    proc2->state = PROCESS_READY;


    // Now you can schedule
    schedule();
}
