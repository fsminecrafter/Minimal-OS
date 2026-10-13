#ifndef BOOT_OPTIONS_H
#define BOOT_OPTIONS_H

#include <stdbool.h>

extern bool kernel_boot_no_disk;
// Set by the --debug boot option; enables per-dispatch scheduler logging.
extern bool kernel_boot_debug;

#endif