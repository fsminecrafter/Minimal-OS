ENTRY(start)

SECTIONS
{
    /* Start at 1MB (Multiboot requirement) */
    . = 1M;

    /* Multiboot header */
    .boot :
    {
        KEEP(*(.multiboot_header))
    }

    /* Constructor section for registering commands */
    .command_ctors ALIGN(8) :
    {
        __start_command_ctors = .;
        KEEP(*(.command_ctors))
        __stop_command_ctors = .;
    }

    /* Kernel code (read-only and executable) */
    .text ALIGN(4K) :
    {
        *(.text)
        *(.text.*)
    }

    /* Read-only data (e.g., strings, const variables) */
    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* Initialized data (global vars with values) */
    .data ALIGN(4K) :
    {
        *(.data)
        *(.data.*)
    }

    /* Uninitialized data (BSS) */
    .bss ALIGN(4K) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }

    /* Optional: provide symbols for start/end of kernel */
    _kernel_start = 1M;
    _kernel_end = .;
}
