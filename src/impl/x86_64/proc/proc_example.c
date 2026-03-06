#include "prochandler.h"
#include "x86_64/scheduler.h"
#include "serial.h"

// Example process functions
void example_worker(void) {
    int count = 0;
    while (1) {
        serial_write_str("[WORKER] Working... ");
        serial_write_dec(count++);
        serial_write_str("\n");
        sleep(1000);
    }
}

void example_monitor(void) {
    int checks = 0;
    while (1) {
        serial_write_str("[MONITOR] System check #");
        serial_write_dec(checks++);
        serial_write_str("\n");
        sleep(2000);
    }
}

void example_logger(void) {
    while (1) {
        serial_write_str("[LOGGER] ");
        serial_write_str(getCurrentProcessName());
        serial_write_str(" is running\n");
        sleep(500);
    }
}

// Example: Basic usage
void prochandler_example_basic(void) {
    serial_write_str("\n=== Process Handler Example ===\n\n");
    
    // Create processes
    process_t* worker = createProcess("worker", example_worker);
    process_t* monitor = createProcess("monitor", example_monitor);
    process_t* logger = createProcess("logger", example_logger);
    
    // List all processes
    listAllProcesses();
    
    // Get some info
    serial_write_str("Total processes: ");
    serial_write_dec(getProcessCount());
    serial_write_str("\n");
    
    serial_write_str("Ready processes: ");
    serial_write_dec(getProcessCountByState(PROCESS_READY));
    serial_write_str("\n\n");
}

// Example: Process control
void prochandler_example_control(void) {
    serial_write_str("\n=== Process Control Example ===\n\n");
    
    // Create a process
    process_t* worker = createProcess("worker", example_worker);
    
    serial_write_str("Worker PID: ");
    serial_write_dec(worker->pid);
    serial_write_str("\n\n");
    
    // Let it run for a bit...
    serial_write_str("Letting worker run for 3 seconds...\n");
    sleep(3000);
    
    // Pause it
    serial_write_str("\nPausing worker...\n");
    pauseProcess(worker->pid);
    
    sleep(2000);
    
    // Unpause it
    serial_write_str("\nUnpausing worker...\n");
    unpauseProcess(worker->pid);
    
    sleep(2000);
    
    // Kill it
    serial_write_str("\nKilling worker...\n");
    killProcess(worker->pid);
    
    listAllProcesses();
}

// Example: Finding processes
void prochandler_example_find(void) {
    serial_write_str("\n=== Process Lookup Example ===\n\n");
    
    // Create some processes
    createProcess("worker1", example_worker);
    createProcess("worker2", example_worker);
    createProcess("monitor", example_monitor);
    
    // Find by name
    process_t* proc = findProcessByName("monitor");
    if (proc) {
        serial_write_str("Found process by name:\n");
        printProcessInfo(proc);
    }
    
    // Find by PID
    process_t* proc2 = findProcessByPID(2);
    if (proc2) {
        serial_write_str("\nFound process by PID 2:\n");
        printProcessInfo(proc2);
    }
    
    // Pause by name
    serial_write_str("\nPausing 'worker1'...\n");
    pauseProcessByName("worker1");
    
    listAllProcesses();
}

// Example: Advanced - Dynamic process management
void prochandler_example_advanced(void) {
    serial_write_str("\n=== Advanced Process Management ===\n\n");
    
    // Start with some base processes
    createProcess("system_monitor", example_monitor);
    createProcess("logger", example_logger);
    
    // Dynamically create and manage workers
    for (int i = 0; i < 5; i++) {
        char name[32];
        // Simulate sprintf: "worker_N"
        name[0] = 'w'; name[1] = 'o'; name[2] = 'r'; name[3] = 'k';
        name[4] = 'e'; name[5] = 'r'; name[6] = '_';
        name[7] = '0' + i;
        name[8] = '\0';
        
        createProcess(name, example_worker);
    }
    
    listAllProcesses();
    
    serial_write_str("\nRunning with all workers...\n");
    sleep(2000);
    
    // Pause odd-numbered workers
    serial_write_str("\nPausing odd-numbered workers...\n");
    for (int i = 1; i < 5; i += 2) {
        pauseProcess(i + 3);  // Workers start at PID 3
    }
    
    listAllProcesses();
    sleep(2000);
    
    // Unpause and kill
    serial_write_str("\nUnpausing and killing all workers...\n");
    for (int i = 0; i < 5; i++) {
        uint64_t pid = i + 3;
        unpauseProcess(pid);
        killProcess(pid);
    }
    
    listAllProcesses();
}

// Example: Real-world usage - Process monitoring
void process_monitor_task(void) {
    while (1) {
        serial_write_str("\n");
        serial_write_str("=== PROCESS MONITOR ===\n");
        serial_write_str("Current: ");
        serial_write_str(getCurrentProcessName());
        serial_write_str(" (PID ");
        serial_write_dec(getCurrentPID());
        serial_write_str(")\n");
        
        serial_write_str("Total: ");
        serial_write_dec(getProcessCount());
        serial_write_str(" processes\n");
        
        serial_write_str("  Ready: ");
        serial_write_dec(getProcessCountByState(PROCESS_READY));
        serial_write_str("\n  Running: ");
        serial_write_dec(getProcessCountByState(PROCESS_RUNNING));
        serial_write_str("\n  Waiting: ");
        serial_write_dec(getProcessCountByState(PROCESS_WAITING));
        serial_write_str("\n  Paused: ");
        serial_write_dec(getProcessCountByState(PROCESS_PAUSED));
        serial_write_str("\n=======================\n");
        
        sleep(5000);  // Monitor every 5 seconds
    }
}

// Main demo that creates a monitoring system
void prochandler_demo_full(void) {
    serial_write_str("\n\n");
    serial_write_str("############################################\n");
    serial_write_str("#  PROCESS HANDLER FULL DEMO\n");
    serial_write_str("############################################\n\n");
    
    // Create system processes
    createProcess("monitor", process_monitor_task);
    createProcess("worker", example_worker);
    createProcess("logger", example_logger);
    
    serial_write_str("System processes created.\n\n");
    listAllProcesses();
    
    serial_write_str("\nStarting scheduler...\n\n");
    schedulerInit();
}