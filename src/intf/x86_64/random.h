#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Kernel entropy source.
 *
 * Nothing in the kernel needed random numbers until userland started
 * generating session keys (see the Deliver port - the client picks a
 * fresh AES-256 key per connection, and a predictable key is the same
 * as no key at all).
 *
 * Design: a SplitMix64 pool reseeded from whatever the platform
 * actually offers. RDRAND when the CPU has it (QEMU exposes it with
 * -cpu host and most named models), otherwise TSC jitter mixed with
 * the RTC and the PIT tick counter. The fallback is NOT
 * cryptographically strong on its own - random_has_hardware() reports
 * which case you are in so callers doing key generation can warn.
 */

void random_init(void);

// True if RDRAND is available and being mixed in.
bool random_has_hardware(void);

// Stir additional entropy into the pool (interrupt timings, key
// presses, packet arrival times - anything the attacker does not
// control completely).
void random_add_entropy(uint64_t value);

uint64_t random_u64(void);

// Fills buf with len random bytes. Always succeeds.
void random_bytes(void* buf, size_t len);

#endif // RANDOM_H
