#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/blockdev.h"

/*
 * Write-only FAT32 builder, used by the installer to make the boot
 * partition (EFI System Partition) that GRUB and UEFI firmware read.
 *
 * It formats a fresh volume and then adds directories and files; files
 * are laid out contiguously. Names must be 8.3 (e.g. "KERNEL.BIN"); they
 * are stored upper case, which FAT readers match case-insensitively.
 * Nothing is read back, so it cannot modify an existing FAT volume.
 */

typedef struct fat32_builder fat32_builder_t;

// Formats dev. Returns NULL on error (volume too small / too big / I/O).
fat32_builder_t* fat32_create(blockdev_t* dev, const char* label,
                              uint32_t volume_id);

// Directory handle of the root.
uint32_t fat32_root(fat32_builder_t* fs);

// Creates a directory, returns its handle (0 on error).
uint32_t fat32_mkdir(fat32_builder_t* fs, uint32_t parent, const char* name);

bool fat32_add_file(fat32_builder_t* fs, uint32_t dir, const char* name,
                    const void* data, uint32_t size);

// Writes the FATs and directories and frees the builder.
bool fat32_finish(fat32_builder_t* fs);
