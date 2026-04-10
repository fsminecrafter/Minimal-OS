#ifndef SAFEINTS_H
#define SAFEINTS_H

#include <stdint.h>
#include <stdbool.h>
#include "exec_trace.h"

extern bool g_irq_real_state;

// Marker bit to indicate this irq_save() actually disabled interrupts.
#define IRQ_SAVE_DID_DISABLE (1ULL << 63)

static inline uint64_t irq_save(const char* file, const char* func, int line) {
    uint64_t flags;

    asm volatile("pushfq; pop %0" : "=r"(flags) :: "memory");

    bool was_enabled = (flags & (1ULL << 9)) != 0;
    if (was_enabled) {
        trace_cli(file, func, line);
        return flags | IRQ_SAVE_DID_DISABLE;
    }

    return flags;
}

static inline void irq_restore(uint64_t flags, const char* file, const char* func, int line) {
    bool should_enable = flags & (1 << 9);
    bool did_disable = (flags & IRQ_SAVE_DID_DISABLE) != 0;

    if (!did_disable) {
        return;
    }

    trace_irq_restore(file, func, line);
}

void trace_assert_irq_consistency(const char* file, const char* func, int line);

#endif //SAFEINTS_H
