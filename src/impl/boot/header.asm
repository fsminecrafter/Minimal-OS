section .multiboot_header
header_start:
	; magic number its literal magic
	dd 0xe85250d6 ; multiboot2
	; architecture
	dd 0 ; protected mode i386
	; header length
	dd header_end - header_start
	; checksum
	dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

	; Request the ACPI old and new RSDP tags from Multiboot2/GRUB.
	dw 1 ; information request tag
	dw 0 ; optional
	dd 16
	dd 14 ; ACPI old RSDP
	dd 15 ; ACPI new RSDP

	; Request the framebuffer information tag if a graphics mode is set.
	dw 1 ; information request tag
	dw 1 ; optional
	dd 12
	dd 8 ; framebuffer information
	dd 0 ; align the next header tag to 8 bytes

	; Request a bootloader-provided linear framebuffer. Optional so a
	; machine without a usable graphics mode can still boot headless.
	dw 5 ; framebuffer header tag
	dw 1 ; optional
	dd 20
	dd 1024
	dd 768
	dd 32
	dd 0 ; align the next header tag to 8 bytes

	; end tag
	dw 0
	dw 0
	dd 8
header_end:
