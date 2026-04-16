# ===== Toolchain configuration =====

# Allow override:
#   make CROSS_PREFIX=x86_64-elf-
#   make CROSS_PREFIX=/opt/cross/bin/x86_64-elf-
CROSS_PREFIX ?= x86_64-elf-

# Auto-detect GitHub Actions toolchain ONLY if not overridden
ifeq ($(origin CROSS_PREFIX), default)
ifneq ($(wildcard $(HOME)/cross/bin/x86_64-elf-gcc),)
CROSS_PREFIX := $(HOME)/cross/bin/x86_64-elf-
endif
endif

CC   ?= $(CROSS_PREFIX)gcc
LD   ?= $(CROSS_PREFIX)ld
AS   ?= nasm
PY   ?= python3
QEMU ?= qemu-system-x86_64

# ===== Flags =====
CFLAGS := -ffreestanding -O2 -Wall -Wextra -I src/intf
LDFLAGS := -nostdlib -ffreestanding -O2

# ===== Source discovery =====
kernel_source_files := $(shell [ -d src/impl/kernel ] && find src/impl/kernel -name '*.c' || true)
kernel_object_files := $(patsubst src/impl/kernel/%.c, build/kernel/%.o, $(kernel_source_files))

x86_64_c_source_files := $(shell [ -d src/impl/x86_64 ] && find src/impl/x86_64 -name '*.c' || true)
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.o, $(x86_64_c_source_files))

x86_64_asm_source_files := $(shell [ -d src/impl/x86_64 ] && find src/impl/x86_64 -name '*.asm' || true)
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.o, $(x86_64_asm_source_files))

x86_64_object_files := $(x86_64_c_object_files) $(x86_64_asm_object_files)

audio_wav_files := $(shell [ -d src/resources ] && find src/resources -name '*.wav' || true)
audio_obj_files_build := $(patsubst src/resources/%.wav, build/resources/%.o, $(audio_wav_files))
audio_obj_files_src := $(patsubst src/resources/%.wav, src/resources/%.o, $(audio_wav_files))
audio_object_files := $(audio_obj_files_build) $(audio_obj_files_src)

# ===== Sanity checks =====
.PHONY: check-tools
check-tools:
	@command -v $(CC) >/dev/null || (echo "Missing compiler: $(CC)" && exit 1)
	@command -v $(AS) >/dev/null || (echo "Missing assembler: $(AS)" && exit 1)
	@command -v $(PY) >/dev/null || (echo "Missing python: $(PY)" && exit 1)
	@command -v grub-mkrescue >/dev/null || (echo "Missing grub-mkrescue (install grub-pc-bin & xorriso)" && exit 1)

# ===== Compile rules =====
build/kernel/%.o: src/impl/kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/x86_64/%.o: src/impl/x86_64/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/x86_64/%.o: src/impl/x86_64/%.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

build/resources/%.o: src/resources/%.wav
	@mkdir -p $(dir $@)
	$(PY) tools/audioconverter/wavtoadi.py --format IADPCM --object-file $< $@

src/resources/%.o: src/resources/%.wav
	$(PY) tools/audioconverter/wavtoheader.py $< $@

# ===== Build =====
.PHONY: build-x86_64
build-x86_64: check-tools $(kernel_object_files) $(x86_64_object_files) $(audio_object_files)
	@mkdir -p dist/x86_64
	$(CC) $(LDFLAGS) -T targets/x86_64/linker.ld \
		-o dist/x86_64/kernel.bin \
		$(kernel_object_files) $(x86_64_object_files) $(audio_object_files)

	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin

	@echo "Creating ISO..."
	grub-mkrescue -o dist/x86_64/kernel.iso targets/x86_64/iso || (echo "ISO creation failed" && exit 1)

# ===== Audio only =====
.PHONY: audio
audio: $(audio_object_files)

# ===== Clean =====
.PHONY: clean
clean:
	rm -rf build dist

# ===== Run targets =====
.PHONY: run
run: build-x86_64
	$(QEMU) -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio \
	-audiodev pa,id=speaker -machine pcspk-audiodev=speaker \
	-usb -device usb-kbd \
	-audiodev pa,id=audio0 -device AC97,audiodev=audio0 \
	-device ahci,id=ahci \
	-drive id=disk0,file=sata256.img,if=none,format=raw \
	-device ide-hd,drive=disk0,bus=ahci.0

# ===== Debug =====
.PHONY: run-debug
run-debug: build-x86_64
	$(QEMU) -cdrom dist/x86_64/kernel.iso -m 1024M -boot d \
	-d guest_errors,int,cpu_reset,unimp -D qemu.log \
	--no-reboot -serial stdio -S -s

# ===== Default =====
.DEFAULT_GOAL := all
.PHONY: all
all: build-x86_64
