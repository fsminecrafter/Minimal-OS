#ifndef EXEC_TRACE_H
#define EXEC_TRACE_H

#include <stdint.h>
#include <stdbool.h>

#define cli() trace_cli(__FILE__, __func__, __LINE__)
#define sti() trace_sti(__FILE__, __func__, __LINE__)

/*
 * Execution Tracer - Debug Tool for MinimalOS
 * 
 * Logs every function entry/exit and line execution to serial port
 * Helps identify exactly where the system hangs or crashes
 * 
 * Usage:
 *   TRACE_ENTER();              // At function start
 *   TRACE_LINE();               // At important lines
 *   TRACE_EXIT();               // Before return
 *   TRACE_VALUE(x);             // Log a value
 *   TRACE_PTR(ptr);             // Log a pointer
 */

// ===========================================
// CONFIGURATION
// ===========================================

#define TRACE_ENABLED 1           // Set to 0 to disable all tracing
#define TRACE_MAX_DEPTH 100       // Max call stack depth
#define TRACE_BUFFER_SIZE 256     // Per-message buffer size

// ===========================================
// TRACE MACROS
// ===========================================

#if TRACE_ENABLED

#define TRACE_ENTER() \
    trace_enter(__FILE__, __FUNCTION__, __LINE__)

#define TRACE_EXIT() \
    trace_exit(__FILE__, __FUNCTION__, __LINE__)

#define TRACE_LINE() \
    trace_line(__FILE__, __FUNCTION__, __LINE__)

#define TRACE_MSG(msg) \
    trace_msg(__FILE__, __FUNCTION__, __LINE__, msg)

#define TRACE_VALUE(name, value) \
    trace_value(__FILE__, __FUNCTION__, __LINE__, name, (uint64_t)(value))

#define TRACE_PTR(name, ptr) \
    trace_ptr(__FILE__, __FUNCTION__, __LINE__, name, (void*)(ptr))

#define TRACE_BOOL(name, val) \
    trace_bool(__FILE__, __FUNCTION__, __LINE__, name, val)

#define TRACE_HEX(name, val) \
    trace_hex(__FILE__, __FUNCTION__, __LINE__, name, (uint64_t)(val))

// Conditional tracing
#define TRACE_IF(cond) \
    if (cond) trace_line(__FILE__, __FUNCTION__, __LINE__)

// Loop progress
#define TRACE_LOOP(i, total) \
    if (((i) % 100) == 0) trace_loop(__FILE__, __FUNCTION__, __LINE__, i, total)

#else

// Disabled - no overhead
#define TRACE_ENTER()
#define TRACE_EXIT()
#define TRACE_LINE()
#define TRACE_MSG(msg)
#define TRACE_VALUE(name, value)
#define TRACE_PTR(name, ptr)
#define TRACE_BOOL(name, val)
#define TRACE_HEX(name, val)
#define TRACE_IF(cond)
#define TRACE_LOOP(i, total)

#endif

// ===========================================
// API FUNCTIONS
// ===========================================

/**
 * Initialize execution tracer
 */
void trace_init(void);

/**
 * Enable/disable tracing
 */
void trace_enable(bool enabled);

/**
 * Check if tracing is enabled
 */
bool trace_is_enabled(void);

/**
 * Enter function
 */
void trace_enter(const char* file, const char* func, int line);

/**
 * Exit function
 */
void trace_exit(const char* file, const char* func, int line);

/**
 * Mark execution line
 */
void trace_line(const char* file, const char* func, int line);

/**
 * Log message
 */
void trace_msg(const char* file, const char* func, int line, const char* msg);

/**
 * Log value
 */
void trace_value(const char* file, const char* func, int line, 
                const char* name, uint64_t value);

/**
 * Log pointer
 */
void trace_ptr(const char* file, const char* func, int line,
              const char* name, void* ptr);

/**
 * Log boolean
 */
void trace_bool(const char* file, const char* func, int line,
               const char* name, bool value);

/**
 * Log hex value
 */
void trace_hex(const char* file, const char* func, int line,
              const char* name, uint64_t value);

/**
 * Log loop progress
 */
void trace_loop(const char* file, const char* func, int line,
               uint32_t current, uint32_t total);

/**
 * Dump call stack
 */
void trace_dump_stack(void);

/**
 * Get current trace depth
 */
uint32_t trace_get_depth(void);

/**
 * Clear trace buffer
 */
void trace_clear(void);

/**
 * Set trace filter (only trace functions matching pattern)
 */
void trace_set_filter(const char* pattern);

/**
 * Clear trace filter
 */
void trace_clear_filter(void);


void trace_dump_registers(void);

// ===========================================
// INTERRUPT TRACE API
// ===========================================

/**
 * Trace + disable interrupts (CLI)
 */
void trace_cli(const char* file, const char* func, int line);

/**
 * Trace + enable interrupts (STI)
 */
void trace_sti(const char* file, const char* func, int line);

/**
 * Trace restore interrupt state (enable = true to STI, false to CLI)
 */
void trace_irq_restore(const char* file, const char* func, int line);

/**
 * Dump interrupt audit stack
 */
void trace_dump_interrupt_audit(void);

#endif // EXEC_TRACE_H
