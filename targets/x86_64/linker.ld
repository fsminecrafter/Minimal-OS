ENTRY(start)

MEMORY
{
    RAM (rwx) : ORIGIN = 1M, LENGTH = 512M
}

SECTIONS
{
    . = 1M;

    /* Multiboot header */
    .boot :
    {
        KEEP(*(.multiboot_header))
    } > RAM

    /* Command constructors */
    .command_ctors ALIGN(8) :
    {
        __start_command_ctors = .;
        KEEP(*(.command_ctors))
        KEEP(*(.command_ctors.*))
        __stop_command_ctors = .;
    } > RAM

    /* Code */
    .text ALIGN(4K) :
    {
        *(.text)
        *(.text.*)
    } > RAM

    /* Read-only data */
    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    } > RAM

    /* Data */
    .data ALIGN(4K) :
    {
        *(.data)
        *(.data.*)
    } > RAM

    /* BSS */
    .bss ALIGN(4K) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }

    _kernel_start = 1M;
    _kernel_end = .;
}