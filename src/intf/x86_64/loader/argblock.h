#ifndef ARGBLOCK_H
#define ARGBLOCK_H

/*
 * The argument block a program starts with: its argv and envp arrays and
 * their strings, laid out at the low end of its stack (runcommand.c). The
 * entry point gets RDI = argc, RSI = argv, RDX = envp; both arrays end with
 * a NULL pointer, as every C runtime expects.
 *
 *   [argv[0] .. argv[argc-1], NULL][envp[0] .. envp[n-1], NULL][strings...]
 *
 * The environment is passed in packed form: "NAME=value\0NAME=value\0", the
 * way the kernel keeps it for each process (process_t.env). No hardware or
 * kernel state here, so tests/host can check it.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Most argv entries and environment entries a program can be given.
#define ARGBLOCK_MAX_ARGS 1024
#define ARGBLOCK_MAX_ENV  1024

// Number of strings in a packed environment (stops at env_len bytes).
uint32_t argblock_env_count(const char* env, uint32_t env_len);

// Bytes the block needs. 0 when there are too many entries.
size_t argblock_size(uint32_t argc, const char* const* argv,
                     const char* env, uint32_t env_len);

// Writes the block into `region` (region_size bytes, 8-byte aligned) as
// the program will see it at `user_base`: every pointer stored is
// user_base + its offset in the region. NULL argv entries become "".
// Returns false when it does not fit.
bool argblock_build(void* region, size_t region_size, uintptr_t user_base,
                    uint32_t argc, const char* const* argv,
                    const char* env, uint32_t env_len,
                    char*** out_argv, char*** out_envp);

#endif // ARGBLOCK_H
