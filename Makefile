CC := /opt/cross/bin/x86_64-elf-gcc
LD := /opt/cross/bin/x86_64-elf-ld

kernel_source_files := $(shell find src/impl/kernel -name *.c)
kernel_object_files := $(patsubst src/impl/kernel/%.c, build/kernel/%.o, $(kernel_source_files))

x86_64_c_source_files := $(shell find src/impl/x86_64 -name *.c)
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.o, $(x86_64_c_source_files))

x86_64_asm_source_files := $(shell find src/impl/x86_64 -name *.asm)
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.o, $(x86_64_asm_source_files))

x86_64_object_files := $(x86_64_c_object_files) $(x86_64_asm_object_files)

build/kernel/%.o: src/impl/kernel/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $(patsubst build/kernel/%.o, src/impl/kernel/%.c, $@) -o $@

build/x86_64/%.o: src/impl/x86_64/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $(patsubst build/x86_64/%.o, src/impl/x86_64/%.c, $@) -o $@

build/x86_64/%.o: src/impl/x86_64/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $(patsubst build/x86_64/%.o, src/impl/x86_64/%.asm, $@) -o $@

.PHONY: build-x86_64
build-x86_64: $(kernel_object_files) $(x86_64_object_files)
	mkdir -p dist/x86_64
	$(LD) -n -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(kernel_object_files) $(x86_64_object_files)
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86_64/kernel.iso targets/x86_64/iso

.PHONY: clean
clean:
	rm -rf build dist

.PHONY: run
run: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd

.PHONY: run-ps2
run: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev pa,id=speaker -machine pcspk-audiodev=speaker

.PHONY: run-int
run-int: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -d int -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd

.PHONY: run-de
run-de: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log --no-reboot -serial stdio -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd

.PHONY: run-dex
run-dex: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log --no-reboot -serial stdio -S -s -usb -device usb-kbd
