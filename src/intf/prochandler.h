#ifndef PROCHANDLER_H
#define PROCHANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "x86_64/proc.h"
#include "x86_64/gpu.h"


// GPU DEVICE ACCESS


// Set the system GPU device (call once after GPU initialization)
void setSystemGPU(gpu_device_t* gpu);

// Get the system GPU device
// Returns: Pointer to GPU device, or NULL if not set
gpu_device_t* getSystemGPU(void);


// PROCESS CREATION


// Create a new process and add it to the scheduler
// Returns: Pointer to created process, or NULL on failure
process_t* createProcess(const char* file_name, void (*entry_point)());


// PROCESS TERMINATION


// Kill a process by PID
// Returns: true if process was killed, false if not found or already dead
bool killProcess(uint64_t pid);

// Kill a process by name
// Returns: true if process was killed, false if not found or already dead
bool killProcessByName(const char* name);


// PROCESS LOOKUP


// Find a process by its PID
// Returns: Pointer to process, or NULL if not found
process_t* findProcessByPID(uint64_t pid);

// Find a process by its name
// Returns: Pointer to process, or NULL if not found
process_t* findProcessByName(const char* name);


// PROCESS PAUSE/UNPAUSE


// Pause a process by PID (stops it from being scheduled)
// Returns: true if paused successfully, false on error
bool pauseProcess(uint64_t pid);

// Unpause a process by PID (allows it to be scheduled again)
// Returns: true if unpaused successfully, false on error
bool unpauseProcess(uint64_t pid);

// Pause a process by name
// Returns: true if paused successfully, false on error
bool pauseProcessByName(const char* name);

// Unpause a process by name
// Returns: true if unpaused successfully, false on error
bool unpauseProcessByName(const char* name);


// PROCESS INFORMATION


// Print detailed information about a process
void printProcessInfo(process_t* proc);

// List all processes in the system
void listAllProcesses(void);


// PROCESS COUNT


// Get total number of processes
int getProcessCount(void);

// Get number of processes in a specific state
int getProcessCountByState(process_state_t state);


// CURRENT PROCESS INFO


// Get the currently running process
process_t* getCurrentProcess(void);

// Get the PID of the currently running process
uint64_t getCurrentPID(void);

// Get the name of the currently running process
const char* getCurrentProcessName(void);

// Get a list of all processes (up to max_procs)
void getprocslist(process_t** buffer, size_t max_procs);
// Get a list of all process names (up to max_procs)
void getprocslistNames(const char** buffer, size_t max_procs);

#endif // PROCHANDLER_H