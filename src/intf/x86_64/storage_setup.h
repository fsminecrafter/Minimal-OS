#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "x86_64/blockdev.h"

// Called with 0-100 and a short description while a long job runs.
typedef void (*storage_progress_fn)(uint32_t percent, const char* text);

// Formats device with MinimaFS and mounts it as drive_number.
bool storage_format_volume(blockdev_t* device, uint8_t drive_number,
                           const char* label);

// Creates etc/, services/, programs/ and System.conf on a mounted drive
// and unpacks the built-in system packages into it.
bool storage_populate_system(uint8_t drive_number, storage_progress_fn progress);

// Unpacks an .mpkg archive in memory into target_dir (created if needed).
bool storage_install_package(const uint8_t* archive, uint32_t archive_size,
                             const char* target_dir);
