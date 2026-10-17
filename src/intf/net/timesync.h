#ifndef NET_TIMESYNC_H
#define NET_TIMESYNC_H

#include <stdint.h>
#include "x86_64/syscall.h"

/*
 * Internet time. A kernel process polls an HTTP(S) address answering
 * with the Unix time in milliseconds and sets the system clock (UTC)
 * from it. Configured from user space through SYS_TIMESYNC.
 */

// Handles SYS_TIMESYNC (CONFIGURE / STATUS) for `request`.
uint64_t timesync_syscall(syscall_timesync_t* request);

// Unix milliseconds -> calendar date and time (UTC). Exposed for tests.
void timesync_unix_ms_to_datetime(uint64_t unix_ms, uint16_t* year,
                                  uint8_t* month, uint8_t* day,
                                  uint8_t* hour, uint8_t* minute,
                                  uint8_t* second);

#endif
