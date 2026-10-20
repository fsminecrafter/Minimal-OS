#ifndef MINIMAFSHANDLER_H
#define MINIMAFSHANDLER_H

#include <stdbool.h>
#include "x86_64/blockdev.h"
#include "x86_64/minimafs.h"

/*
 * Glue between block devices and MinimaFS drive numbers.
 *
 * A MinimaFS "device" is simply the blockdev_t it lives on: a whole disk
 * (the original layout, filesystem at sector 0) or a partition.
 */
typedef blockdev_t minimafs_disk_device_t;

// Probes all disks and picks the boot volume (see storage_boot_volume()).
// driveindex is kept for the old initdisk command and ignored.
minimafs_disk_device_t* initializeminimafs(int driveindex);

// The volume mounted (or to be mounted) as 0:. Falls back to the first
// disk when no MinimaFS volume exists yet, so 'format' has a target.
minimafs_disk_device_t* getminimadrive(void);

// Mounts device as drive number deviceindex. Returns minimafs_mount()'s
// code: 1 = ok, 2 = already mounted, 3/4/5 = bad volume, 0 = error.
int mountdrive(minimafs_disk_device_t* device, int deviceindex);

// Mounts every other MinimaFS volume as 1:, 2:, ... Returns how many.
uint32_t storage_automount_others(void);

// Lowest free drive number >= first, or -1.
int storage_free_drive_number(int first);

// Kernel command line "root=" value (partition name or GPT GUID), set by
// kernel_main before the disks are probed.
extern char kernel_boot_root[64];

#endif // MINIMAFSHANDLER_H
