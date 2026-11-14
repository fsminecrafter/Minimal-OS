#ifndef BOOT_OPTIONS_H
#define BOOT_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>

extern bool kernel_boot_no_disk;
// 'ramfs' / 'ramfs=<MiB>': the system volume 0: is a fresh RAM disk with
// the system files unpacked into it. Real disks are never probed, so
// nothing reads or writes them; all changes are lost at the next boot.
extern bool kernel_boot_ramfs;
extern uint32_t kernel_boot_ramfs_mib;
// 'serial-echo': everything the terminal prints is copied to COM1 too, so
// a headless VM's serial log shows command output (tools/hyperv/mosvm.sh).
extern bool kernel_boot_serial_echo;
// OS-3: threads of one program run on any CPU (default). The boot flag
// 'threads1cpu' pins every thread group to its owner's CPU as before.
extern bool kernel_boot_threads_multicore;
// Set by the --debug boot option; enables per-dispatch scheduler logging.
extern bool kernel_boot_debug;

#endif