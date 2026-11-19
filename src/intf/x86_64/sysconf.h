#pragma once
/*
 * 0:/etc/System.conf: "Key=value" lines (KeyboardLayout, Resolution,
 * BootupAnim, ...). Keys are matched without regard to case.
 */
#include <stdbool.h>
#include <stddef.h>

// The value of `key` into out; false when the file or the key is missing.
bool sysconf_get(const char* key, char* out, size_t cap);
// Sets `key` (replacing its line, or appending one), keeping every other
// line. Creates the file when it is missing. False when the disk is not
// mounted or the write fails.
bool sysconf_set(const char* key, const char* value);
