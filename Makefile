# ===== Toolchain configuration =====
# Allow override from environment:
#   make CROSS_PREFIX=x86_64-elf-
#   make CROSS_PREFIX=/opt/cross/bin/x86_64-elf-
CROSS_PREFIX ?= x86_64-elf-

CC   := $(CROSS_PREFIX)gcc
LD   := $(CROSS_PREFIX)ld
AS   := nasm
PY   := python3

# Optional: allow overriding tools completely
# make CC=clang LD=ld.lld
# make AS=yasm

# ===== Source discovery =====
kernel_source_files := $(shell find src/impl/kernel -name '*.c')
kernel_object_files := $(patsubst src/impl/kernel/%.c, build/kernel/%.o, $(kernel_source_files))

x86_64_c_source_files := $(shell find src/impl/x86_64 -name '*.c')
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.o, $(x86_64_c_source_files))

x86_64_asm_source_files := $(shell find src/impl/x86_64 -name '*.asm')
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.o, $(x86_64_asm_source_files))

x86_64_object_files := $(x86_64_c_object_files) $(x86_64_asm_object_files)

audio_wav_files := $(shell find src/resources -name '*.wav')
audio_obj_files_src := $(shell find src/resources -name '*.o')
audio_obj_files_build := $(patsubst src/resources/%.wav, build/resources/%.o, $(audio_wav_files))
audio_object_files := $(audio_obj_files_build) $(audio_obj_files_src)

# ===== Compile rules =====
build/kernel/%.o: src/impl/kernel/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $< -o $@

build/x86_64/%.o: src/impl/x86_64/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $< -o $@

build/x86_64/%.o: src/impl/x86_64/%.asm
	mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

build/resources/%.o: src/resources/%.wav
	mkdir -p $(dir $@)
	$(PY) tools/audioconverter/wavtoadi.py --format IADPCM --object-file $< $@

src/resources/%.o: src/resources/%.wav
	$(PY) tools/audioconverter/wavtoheader.py $< $@

# ===== Build =====
.PHONY: build-x86_64
build-x86_64: $(kernel_object_files) $(x86_64_object_files) $(audio_object_files)
	mkdir -p dist/x86_64
	$(LD) -n -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld \
		$(kernel_object_files) $(x86_64_object_files) $(audio_object_files)
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue -o dist/x86_64/kernel.iso targets/x86_64/iso

# ===== Utils =====
.PHONY: clean
clean:
	rm -rf build dist

.PHONY: audio
audio: $(audio_obj_files_build) $(audio_obj_files_src)

# ===== Run =====
QEMU := qemu-system-x86_64

.PHONY: run
run: build-x86_64
	$(QEMU) -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio \
	-audiodev pa,id=speaker -machine pcspk-audiodev=speaker \
	-usb -device usb-kbd \
	-audiodev pa,id=audio0 -device AC97,audiodev=audio0 \
	-device ahci,id=ahci \
	-drive id=disk0,file=sata256.img,if=none,format=raw \
	-device ide-hd,drive=disk0,bus=ahci.0

.PHONY: clean
clean:
	rm -rf build dist

.PHONY: audio
audio: $(audio_obj_files_build) $(audio_obj_files_src)

src/resources/%.o: src/resources/%.wav
	python3 tools/audioconverter/wavtoheader.py $< $@

.PHONY: run
run: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd -audiodev pa,id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0

.PHONY: run-sdl
run-sdl: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev sdl,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd -audiodev sdl,id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0

.PHONY: run-alsa
run-alsa: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev alsa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd -audiodev alsa,id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0

.PHONY: run-ps2
run-ps2: build-x86_64
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
