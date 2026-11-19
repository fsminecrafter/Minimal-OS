#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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

// Choices the installer makes for the next storage_populate_system().
// Both reset after it runs, so 'format' and RamFS boots get the defaults
// (every app on the install media, no repositories).
//
// apps: comma-separated app names from the install media ("Doom,feplayer"),
//       "" for none, NULL for all of them. An app is a top-level entry of
//       the "mosgames" boot module with ".run" and any folder of the same
//       name: "Doom" is Doom.run plus Doom/.
// repos: newline-separated Deliver servers ("host:port"), written to
//       N:/etc/dlr/repos. 'dlr' adds each one the first time it can reach
//       it (it resolves names and probes the server, which needs the
//       network the installed system has, not this one).
void storage_setup_choose(const char* apps, const char* repos);

// Writes "name bytes\n" for each app on the install media into out
// (NUL-terminated). Returns how many there are; 0 without the module.
int storage_setup_list_apps(char* out, size_t out_size);
