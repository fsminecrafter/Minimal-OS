CC := $(HOME)/cross/bin/x86_64-elf-gcc
LD := $(HOME)/cross/bin/x86_64-elf-ld

module_c_source_files := $(shell find src/modules -name '*.c')
module_c_object_files := $(patsubst src/modules/%.c, build/modules/%.o, $(module_c_source_files))

module_asm_source_files := $(shell find src/modules -name '*.asm')
module_asm_object_files := $(patsubst src/modules/%.asm, build/modules/%.o, $(module_asm_source_files))

impl_c_source_files := $(shell find src/impl -name '*.c')
impl_c_object_files := $(patsubst src/impl/%.c, build/impl/%.o, $(impl_c_source_files))

impl_asm_source_files := $(shell find src/impl -name '*.asm')
impl_asm_object_files := $(patsubst src/impl/%.asm, build/impl/%.o, $(impl_asm_source_files))

source_object_files := $(module_c_object_files) $(module_asm_object_files) \
	$(impl_c_object_files) $(impl_asm_object_files)

audio_wav_files := $(shell find src/resources -name '*.wav')
audio_obj_files_src := $(shell find src/resources -name '*.o')
audio_obj_files_build := $(patsubst src/resources/%.wav, build/resources/%.o, $(audio_wav_files))
audio_object_files := $(audio_obj_files_build) $(audio_obj_files_src)

build/modules/%.o: src/modules/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $< -o $@

build/modules/%.o: src/modules/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

build/impl/%.o: src/impl/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $< -o $@

build/impl/%.o: src/impl/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

build/resources/%.o: src/resources/%.wav
	mkdir -p $(dir $@)
	python3 tools/audioconverter/wavtoadi.py --format IADPCM --object-file $< $@

.PHONY: build-x86_64
build-x86_64: $(source_object_files) $(audio_object_files) 
	mkdir -p dist/x86_64
	$(LD) -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(source_object_files) $(audio_object_files)
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86_64/kernel.iso targets/x86_64/iso

.PHONY: clean
clean:
	rm -rf build dist

.PHONY: audio
audio: $(audio_obj_files_build) $(audio_obj_files_src)

src/resources/%.o: src/resources/%.wav
	python3 tools/audioconverter/wavtoheader.py $< $@

.PHONY: run
run: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log -serial stdio -usb -device usb-kbd -audiodev pa,id=speaker -machine pcspk-audiodev=speaker  -audiodev pa,id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0

.PHONY: run-audio
run-audio: build-x86_64
	qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log  -usb -device usb-kbd -serial stdio -audiodev wav,id=speaker,path=speaker.wav -machine pcspk-audiodev=speaker -audiodev wav,id=audio0,path=ac97.wav -device AC97,audiodev=audio0,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0

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

.PHONY: ci-test
ci-test:
	bash tools/debug/ci_test.sh

.PHONY: ci-interact
ci-interact:
	bash tools/debug/ci_interact.sh
