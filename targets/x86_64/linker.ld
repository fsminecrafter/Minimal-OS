ENTRY(start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    /* Multiboot header must be early */
    .boot :
    {
        KEEP(*(.multiboot_header))
    }

    /* Code */
    .text ALIGN(4K) :
    {
        *(.text)
        *(.text.*)
    }

    /* Read-only data */
    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* Command constructors */
    .command_ctors ALIGN(8) :
    {
        __start_command_ctors = .;
        KEEP(*(.command_ctors))
        KEEP(*(.command_ctors.*))
        __stop_command_ctors = .;
    }

    /* Writable data */
    .data ALIGN(4K) :
    {
        *(.data)
        *(.data.*)
    }

    /* BSS */
    .bss ALIGN(4K) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }

    _kernel_end = .;
}