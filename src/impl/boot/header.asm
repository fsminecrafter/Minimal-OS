section .multiboot_header
header_start:
	; magic number
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

	; end tag
	dw 0
	dw 0
	dd 8
header_end:
