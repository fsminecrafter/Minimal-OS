#pragma once
#include <stdint.h>
#include "x86_64/blockdev.h"

#define RAMDISK_DEFAULT_MIB 64u
#define RAMDISK_MIN_MIB     16u

// Registers a zeroed RAM disk of `mib` MiB (less if the heap is short,
// never below RAMDISK_MIN_MIB) as block device "ram0". NULL on failure
// or if one already exists.
blockdev_t* ramdisk_create(uint32_t mib);
