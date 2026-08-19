#include "x86_64/scheduler.h"
#include "x86_64/proc.h"
#include "x86_64/gpu.h"
#include "serial.h"
#include "panic.h"

// ===========================================
// GPU DEVICE ACCESS
// ===========================================

static gpu_device_t* g_system_gpu = NULL;

void setSystemGPU(gpu_device_t* gpu) {
    g_system_gpu = gpu;
    if (gpu) {
        serial_write_str("[PROC] System GPU set: ");
        serial_write_dec(gpu->width);
        serial_write_str("x");
        serial_write_dec(gpu->height);
        serial_write_str("\n");
    }
}

gpu_device_t* getSystemGPU(void) {
    return g_system_gpu;
}

// ===========================================
// PROCESS CREATION
// ===========================================

process_t* createProcess(const char* file_name, void (*entry_point)()) {
    if (!file_name || !entry_point) {
        serial_write_str("[PROC] Error: Invalid parameters for createProcess\n");
        return NULL;
    }
    
    process_t* proc = proc_create(file_name, entry_point);
    if (!proc) {
        serial_write_str("[PROC] Error: Failed to create process ");
        serial_write_str(file_name);
        serial_write_str("\n");
        return NULL;
    }
    
    proc->state = PROCESS_READY;
    
    serial_write_str("[PROC] Created process: ");
    serial_write_str(proc->name);
    serial_write_str("\n");
    
    return proc;
}

// ===========================================
// PROCESS TERMINATION
// ===========================================

bool killProcess(uint64_t pid) {
    if (pid == 0) {
        serial_write_str("[PROC] Error: Cannot kill process with PID 0\n");
        return false;
    }
    
    // Find process by PID
    process_t* proc = NULL;
    for (process_t* p = proc_list_head; p != NULL; p = p->next) {
        if (p->pid == pid) {
            proc = p;
            break;
        }
    }
    
    if (!proc) {
        serial_write_str("[PROC] Error: Process with PID ");
        serial_write_dec(pid);
        serial_write_str(" not found\n");
        return false;
    }
    
    // Don't kill if already dead
    if (proc->state == PROCESS_ZOMBIE || proc->state == PROCESS_TERMINATED) {
        serial_write_str("[PROC] Warning: Process ");
        serial_write_str(proc->name);
        serial_write_str(" is already terminated\n");
        return false;
    }
    
    // Mark as zombie (scheduler will clean up)
    proc->state = PROCESS_ZOMBIE;
    
    serial_write_str("[PROC] Killed process: ");
    serial_write_str(proc->name);
    serial_write_str(" (PID ");
    serial_write_dec(pid);
    serial_write_str(")\n");
    
    // If we just killed the current process, reschedule
    if (proc == current_process) {
        serial_write_str("[PROC] Current process killed, rescheduling...\n");
        schedule();
    }
    
    return true;
}

bool killProcessByName(const char* name) {
    if (!name) {
        serial_write_str("[PROC] Error: Invalid name\n");
        return false;
    }
    
    process_t* proc = get_proc_by_name(name);
    if (!proc) {
        serial_write_str("[PROC] Error: Process '");
        serial_write_str(name);
        serial_write_str("' not found\n");
        return false;
    }
    
    return killProcess(proc->pid);
}

// ===========================================
// PROCESS LOOKUP
// ===========================================

process_t* findProcessByPID(uint64_t pid) {
    for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
        if (proc->pid == pid) {
            return proc;
        }
    }
    return NULL;
}

process_t* findProcessByName(const char* name) {
    if (!name) return NULL;
    return get_proc_by_name(name);
}

// ===========================================
// PROCESS PAUSE/UNPAUSE
// ===========================================

bool pauseProcess(uint64_t pid) {
    process_t* proc = findProcessByPID(pid);
    
    if (!proc) {
        serial_write_str("[PROC] Error: Process with PID ");
        serial_write_dec(pid);
        serial_write_str(" not found\n");
        return false;
    }
    
    // Can't pause if already paused, terminated, or zombie
    if (proc->state == PROCESS_PAUSED) {
        serial_write_str("[PROC] Warning: Process ");
        serial_write_str(proc->name);
        serial_write_str(" is already paused\n");
        return false;
    }
    
    if (proc->state == PROCESS_ZOMBIE || proc->state == PROCESS_TERMINATED) {
        serial_write_str("[PROC] Error: Cannot pause terminated process\n");
        return false;
    }
    
    // Save old state in case we need it (not used currently)
    proc->state = PROCESS_PAUSED;
    
    serial_write_str("[PROC] Paused process: ");
    serial_write_str(proc->name);
    serial_write_str(" (PID ");
    serial_write_dec(pid);
    serial_write_str(")\n");
    
    // If we just paused the current process, reschedule
    if (proc == current_process) {
        serial_write_str("[PROC] Current process paused, rescheduling...\n");
        schedule();
    }
    
    return true;
}

bool unpauseProcess(uint64_t pid) {
    process_t* proc = findProcessByPID(pid);
    
    if (!proc) {
        serial_write_str("[PROC] Error: Process with PID ");
        serial_write_dec(pid);
        serial_write_str(" not found\n");
        return false;
    }
    
    // Can only unpause if currently paused
    if (proc->state != PROCESS_PAUSED) {
        serial_write_str("[PROC] Warning: Process ");
        serial_write_str(proc->name);
        serial_write_str(" is not paused (state=");
        serial_write_dec((uint64_t)proc->state);
        serial_write_str(")\n");
        return false;
    }
    
    // Set to READY so scheduler picks it up
    proc->state = PROCESS_READY;
    
    serial_write_str("[PROC] Unpaused process: ");
    serial_write_str(proc->name);
    serial_write_str(" (PID ");
    serial_write_dec(pid);
    serial_write_str(")\n");
    
    return true;
}

bool pauseProcessByName(const char* name) {
    process_t* proc = findProcessByName(name);
    if (!proc) {
        serial_write_str("[PROC] Error: Process '");
        serial_write_str(name);
        serial_write_str("' not found\n");
        return false;
    }
    return pauseProcess(proc->pid);
}

bool unpauseProcessByName(const char* name) {
    process_t* proc = findProcessByName(name);
    if (!proc) {
        serial_write_str("[PROC] Error: Process '");
        serial_write_str(name);
        serial_write_str("' not found\n");
        return false;
    }
    return unpauseProcess(proc->pid);
}

// ===========================================
// PROCESS INFORMATION
// ===========================================

void printProcessInfo(process_t* proc) {
    if (!proc) {
        serial_write_str("[PROC] Error: NULL process\n");
        return;
    }
    
    serial_write_str("Process Information:\n");
    serial_write_str("  Name: ");
    serial_write_str(proc->name);
    serial_write_str("\n  PID: ");
    serial_write_dec(proc->pid);
    serial_write_str("\n  State: ");
    
    switch (proc->state) {
        case PROCESS_READY:      serial_write_str("READY"); break;
        case PROCESS_RUNNING:    serial_write_str("RUNNING"); break;
        case PROCESS_WAITING:    serial_write_str("WAITING"); break;
        case PROCESS_PAUSED:     serial_write_str("PAUSED"); break;
        case PROCESS_ZOMBIE:     serial_write_str("ZOMBIE"); break;
        case PROCESS_TERMINATED: serial_write_str("TERMINATED"); break;
        default:                 serial_write_str("UNKNOWN"); break;
    }
    
    serial_write_str("\n  Entry Point: 0x");
    serial_write_hex(proc->regs[7]);
    serial_write_str("\n  Stack Pointer: 0x");
    serial_write_hex(proc->regs[6]);
    serial_write_str("\n");
}

void listAllProcesses(void) {
    serial_write_str("\n");
    serial_write_str("========================================\n");
    serial_write_str("         PROCESS LIST\n");
    serial_write_str("========================================\n");
    serial_write_str(" PID | State    | Name\n");
    serial_write_str("----------------------------------------\n");
    
    int count = 0;
    for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
        count++;
        
        // Print PID (right-aligned, 4 chars)
        serial_write_str(" ");
        if (proc->pid < 10) serial_write_str("   ");
        else if (proc->pid < 100) serial_write_str("  ");
        else if (proc->pid < 1000) serial_write_str(" ");
        serial_write_dec(proc->pid);
        
        serial_write_str(" | ");
        
        // Print state (9 chars)
        switch (proc->state) {
            case PROCESS_READY:      serial_write_str("READY    "); break;
            case PROCESS_RUNNING:    serial_write_str("RUNNING  "); break;
            case PROCESS_WAITING:    serial_write_str("WAITING  "); break;
            case PROCESS_PAUSED:     serial_write_str("PAUSED   "); break;
            case PROCESS_ZOMBIE:     serial_write_str("ZOMBIE   "); break;
            case PROCESS_TERMINATED: serial_write_str("TERM     "); break;
            default:                 serial_write_str("UNKNOWN  "); break;
        }
        
        serial_write_str(" | ");
        serial_write_str(proc->name);
        serial_write_str("\n");
    }
    
    serial_write_str("----------------------------------------\n");
    serial_write_str("Total processes: ");
    serial_write_dec(count);
    serial_write_str("\n");
    serial_write_str("========================================\n\n");
}

// ===========================================
// PROCESS COUNT
// ===========================================

int getProcessCount(void) {
    int count = 0;
    for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
        count++;
    }
    return count;
}

int getProcessCountByState(process_state_t state) {
    int count = 0;
    for (process_t* proc = proc_list_head; proc != NULL; proc = proc->next) {
        if (proc->state == state) {
            count++;
        }
    }
    return count;
}

void getprocslist(process_t** buffer, size_t max_procs) {
    size_t count = 0;
    for (process_t* proc = proc_list_head; proc != NULL && count < max_procs; proc = proc->next) {
        buffer[count++] = proc;
    }
}

void getprocslistNames(const char** buffer, size_t max_procs) {
    size_t count = 0;
    for (process_t* proc = proc_list_head; proc != NULL && count < max_procs; proc = proc->next) {
        buffer[count++] = proc->name;
    }
}

// ===========================================
// CURRENT PROCESS INFO
// ===========================================

process_t* getCurrentProcess(void) {
    return current_process;
}

uint64_t getCurrentPID(void) {
    return current_process ? current_process->pid : 0;
}

const char* getCurrentProcessName(void) {
    return current_process ? current_process->name : "none";
}