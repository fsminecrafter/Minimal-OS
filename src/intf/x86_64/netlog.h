#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Network log: sends every kernel log line (everything written through
 * serial_write, including program output and app logs) to a PC on the
 * LAN over UDP. Enabled by the kernel argument netlog=<ip>[:<port>],
 * which 'make netboot' adds; the netboot script prints what arrives.
 *
 * Lines are kept in a buffer from the first moment of boot, so the
 * early boot log is sent too once the network is up.
 */

#define NETLOG_DEFAULT_PORT 5140

// Called for every character written to the kernel log. Safe from any
// context (interrupts, other cores); never blocks.
void netlog_putc(char c);

// Reads netlog= from the kernel arguments. Call early in boot.
void netlog_configure(const char* arg);

// Starts the sender process. Call once the scheduler can run processes.
void netlog_start(void);

bool netlog_enabled(void);
