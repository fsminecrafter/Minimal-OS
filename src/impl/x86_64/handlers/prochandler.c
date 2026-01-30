#include "x86_64/scheduler.h"
#include "x86_64/proc.h"

void createProcess(const char* file_name, void (*entry_point)()) {
    process_t* proc = proc_create(file_name, entry_point);
    proc->state = PROCESS_READY;
    schedule();
}