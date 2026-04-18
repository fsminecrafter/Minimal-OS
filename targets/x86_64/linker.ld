ENTRY(start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* R + X */
    data PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    /* Multiboot header must be early */
    .boot :
    {
        KEEP(*(.multiboot_header))
    } :text

    /* Code */
    .text ALIGN(4K) :
    {
        *(.text)
        *(.text.*)
    } :text

    /* Read-only data */
    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    } :text

    /* Command constructors */
    .command_ctors ALIGN(8) :
    {
        __start_command_ctors = .;
        KEEP(*(.command_ctors))
        KEEP(*(.command_ctors.*))
        __stop_command_ctors = .;
    } :text

    /* Writable data */
    .data ALIGN(4K) :
    {
        *(.data)
        *(.data.*)
    } :data

    /* BSS */
    .bss ALIGN(4K) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }

    _kernel_end = .;
}
