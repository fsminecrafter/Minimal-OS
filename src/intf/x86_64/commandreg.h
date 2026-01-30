#pragma once

// Define function pointer type for command constructors
typedef void (*command_registrar_t)(void);

// Place a function pointer into the .command_ctors section
#define REGISTER_COMMAND(fn) \
    static command_registrar_t _reg_##fn __attribute__((used, section(".command_ctors"))) = fn;
