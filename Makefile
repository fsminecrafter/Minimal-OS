toolchain_home := $(if $(SUDO_USER),$(shell getent passwd $(SUDO_USER) | cut -d: -f6),$(HOME))
CC := $(toolchain_home)/cross/bin/x86_64-elf-gcc
LD := $(toolchain_home)/cross/bin/x86_64-elf-ld
BUILD_DATE := $(shell date '+%Y-%m-%d %H:%M:%S')
BUILD_DATE_CFLAG := '-DBUILD_DATE="$(BUILD_DATE)"'
# Interrupts taken in ring 0 push their frame onto the interrupted stack,
# which is exactly where the SysV red zone keeps a leaf function's locals.
KERNEL_CFLAGS := -ffreestanding -mno-red-zone

QEMU_NETDEV ?= user,id=net0
QEMU_TWO_MCAST ?= 230.0.0.1:12345
QEMU_AUDIO_DRIVER ?= none
HOSTCC ?= gcc
HOST_CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I src/intf
MOSDESK_REPO ?= https://github.com/fsminecrafter/MOSdesk.git
# The MOSdesk submodule; override to build some other checkout.
MOSDESK_DIR ?= MOSdesk
# Package builder: compresses on every core and skips unchanged packages.
MKPKG_JOBS ?= $(shell nproc 2>/dev/null || echo 1)
MKPKG := python3 tools/mkpkg/mkpkg.py -j$(MKPKG_JOBS)

.DEFAULT_GOAL := build-x86_64

# Include module configuration
-include config.mk

# Discover module source files dynamically and filter them by their generated config key.
module_config_key = MODULE_$(subst .,_,$(subst /,_,$(1)))
module_files_relative := $(shell find src/modules -type f \( -name '*.c' -o -name '*.asm' \) -printf '%P\n' | sort)
module_source_files := $(foreach file,$(module_files_relative),$(if $(filter 0,$($(call module_config_key,$(file)))),,src/modules/$(file)))
module_c_source_files := $(filter %.c,$(module_source_files))
module_c_object_files := $(patsubst src/modules/%.c, build/modules/%.o, $(module_c_source_files))

module_asm_source_files := $(filter %.asm,$(module_source_files))
module_asm_object_files := $(patsubst src/modules/%.asm, build/modules/%.o, $(module_asm_source_files))

impl_c_source_files := $(shell find src/impl -name '*.c')
impl_c_object_files := $(patsubst src/impl/%.c, build/impl/%.o, $(impl_c_source_files))

minimassl_kernel_sources := \
	Minimal-OS-SDK/libraries/minimaSSL/src/crypto.c \
	Minimal-OS-SDK/libraries/minimaSSL/src/sha.c \
	Minimal-OS-SDK/libraries/minimaSSL/src/tls_gcm.c
minimassl_kernel_objects := $(patsubst Minimal-OS-SDK/libraries/minimaSSL/src/%.c, build/minimaSSL/%.o, $(minimassl_kernel_sources))
minimassl_slib := dist/x86_64/minimaSSL.slib
install1_source := src/resources/install1
install1_mpkg := build/resources/install1.mpkg
install1_object := build/resources/install1_mpkg.o
install2_base_source := src/resources/install2
install2_source := $(install2_base_source)
install2_mpkg := build/resources/install2.mpkg
install2_object := build/resources/install2_mpkg.o
dlr_source_files := $(shell find Minimal-OS-SDK/programs/dlr -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null)
dlr_run := $(install2_source)/dlr.run
dlrconnect_run := $(install2_source)/dlrconnect.run
# FarcadeGL demo (SDK programs/fglcube), installed as 0:/programs/fglcube.run
fglcube_run := $(install2_base_source)/fglcube.run
fglcube_sources := $(wildcard Minimal-OS-SDK/programs/fglcube/*.c) \
	Minimal-OS-SDK/include/farcadegl.h Minimal-OS-SDK/include/fgpu_abi.h
install3_base_source := src/resources/install3
install3_source := $(install3_base_source)
install3_mpkg := build/resources/install3.mpkg
install3_object := build/resources/install3_mpkg.o

ifeq ($(MOSDESK),1)
install2_source := build/resources/mosdesk-install/install2
install3_source := build/resources/mosdesk-install/install3
mosdesk_install_dep := mosdesk-stage
endif

# The packages are rebuilt when switching between plain and MOSDESK=1 builds.
build_mode := $(if $(filter 1,$(MOSDESK)),mosdesk,plain)
mode_stamp := build/resources/mode-$(build_mode).stamp
$(mode_stamp):
	mkdir -p $(dir $@)
	rm -f build/resources/mode-*.stamp
	touch $@

install2_files := $(shell find $(install2_source) -type f 2>/dev/null)
install3_files := $(shell find $(install3_source) -type f 2>/dev/null)
install2_tree_deps := $(install2_source) $(install2_files)
install3_tree_deps := $(install3_source) $(install3_files)

# In MOSdesk mode the install trees are created by mosdesk-stage, so they
# cannot be listed as prerequisites (make -j would look for them too early).
ifeq ($(MOSDESK),1)
install2_tree_deps :=
install3_tree_deps :=
endif

impl_asm_source_files := $(filter-out src/impl/boot/ap_trampoline.asm, $(shell find src/impl -name '*.asm'))
impl_asm_object_files := $(patsubst src/impl/%.asm, build/impl/%.o, $(impl_asm_source_files))

ap_trampoline_bin := build/impl/boot/ap_trampoline.bin
ap_trampoline_object := build/impl/boot/ap_trampoline.o

source_object_files := $(module_c_object_files) $(module_asm_object_files) \
	$(impl_c_object_files) $(impl_asm_object_files) $(ap_trampoline_object) \
	$(minimassl_kernel_objects) $(install1_object) $(install2_object) $(install3_object)

audio_player_enabled := $(if $(filter 0,$(MODULE_Audio_audio_c)),,1)
audio_wav_files := $(if $(audio_player_enabled),$(shell find src/resources -name '*.wav'),)
audio_obj_files_src := $(if $(audio_player_enabled),$(shell find src/resources -name '*.o'),)
audio_obj_files_build := $(patsubst src/resources/%.wav, build/resources/%.o, $(audio_wav_files))
audio_object_files := $(audio_obj_files_build) $(audio_obj_files_src)
header_files := $(shell find src/intf -name '*.h')

.PHONY: FORCE
FORCE:

build/modules/%.o: src/modules/%.c $(header_files)
	mkdir -p $(dir $@)
	$(CC) $(BUILD_DATE_CFLAG) -c -I src/intf $(KERNEL_CFLAGS) $< -o $@

build/modules/%.o: src/modules/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

build/impl/%.o: src/impl/%.c $(header_files)
	mkdir -p $(dir $@)
	$(CC) $(BUILD_DATE_CFLAG) -c -I src/intf -I Minimal-OS-SDK/libraries/minimaSSL/include $(KERNEL_CFLAGS) $< -o $@

build/impl/kernel/main.o: FORCE

build/minimaSSL/%.o: Minimal-OS-SDK/libraries/minimaSSL/src/%.c $(header_files)
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf \
		-I Minimal-OS-SDK/libraries/minimaSSL/include \
		-I Minimal-OS-SDK/libraries/minimaSSL/src \
		$(KERNEL_CFLAGS) -fno-builtin $< -o $@

build/impl/%.o: src/impl/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

$(ap_trampoline_bin): src/impl/boot/ap_trampoline.asm
	mkdir -p $(dir $@)
	nasm -f bin $< -o $@

$(ap_trampoline_object): $(ap_trampoline_bin)
	objcopy -I binary -O elf64-x86-64 -B i386 \
		--redefine-sym _binary_build_impl_boot_ap_trampoline_bin_start=_binary_ap_trampoline_bin_start \
		--redefine-sym _binary_build_impl_boot_ap_trampoline_bin_end=_binary_ap_trampoline_bin_end \
		$< $@

build/resources/%.o: src/resources/%.wav
	mkdir -p $(dir $@)
	python3 tools/audioconverter/wavtoadi.py --format IADPCM --object-file $< $@

.PHONY: minimaSSL-slib
minimaSSL-slib: Minimal-OS-SDK/libraries/minimaSSL/src/crypto.c \
	Minimal-OS-SDK/libraries/minimaSSL/src/evp.c \
	Minimal-OS-SDK/libraries/minimaSSL/src/rand.c \
	Minimal-OS-SDK/libraries/minimaSSL/src/sha.c \
	Minimal-OS-SDK/libraries/minimaSSL/src/tls_gcm.c
	CC=$(CC) LD=$(LD) Minimal-OS-SDK/build.sh --slib Minimal-OS-SDK/libraries/minimaSSL
	mkdir -p $(dir $(minimassl_slib))
	cp Minimal-OS-SDK/minimaSSL.slib $(minimassl_slib)

$(install1_source)/minimaSSL.slib: minimaSSL-slib
	mkdir -p $(install1_source)
	cp $(minimassl_slib) $@

$(install1_mpkg): $(install1_source)/minimaSSL.slib tools/mkpkg/mkpkg.py
	mkdir -p $(dir $@)
	$(MKPKG) $(install1_source) $@ --store

$(install1_object): $(install1_mpkg)
	mkdir -p $(dir $@)
	objcopy -I binary -O elf64-x86-64 -B i386 \
		--redefine-sym _binary_build_resources_install1_mpkg_start=_binary_install1_mpkg_start \
		--redefine-sym _binary_build_resources_install1_mpkg_end=_binary_install1_mpkg_end \
		$< $@

$(install2_mpkg): $(mode_stamp) $(mosdesk_install_dep) $(install2_tree_deps) $(dlr_run) $(dlrconnect_run) $(fglcube_run) tools/mkpkg/mkpkg.py
	mkdir -p $(dir $@)
	$(MKPKG) $(install2_source) $@ --store

$(dlr_run): $(dlr_source_files) Minimal-OS-SDK/build.sh Minimal-OS-SDK/crt0.c Minimal-OS-SDK/link.ld
	CC=$(CC) LD=$(LD) Minimal-OS-SDK/build.sh Minimal-OS-SDK/programs/dlr

$(fglcube_run): $(fglcube_sources) Minimal-OS-SDK/build.sh Minimal-OS-SDK/crt0.c Minimal-OS-SDK/link.ld
	CC=$(CC) LD=$(LD) Minimal-OS-SDK/build.sh Minimal-OS-SDK/programs/fglcube --output $(CURDIR)/$@

$(dlrconnect_run): $(dlr_source_files) Minimal-OS-SDK/build.sh Minimal-OS-SDK/crt0.c Minimal-OS-SDK/link.ld
	CC=$(CC) LD=$(LD) Minimal-OS-SDK/build.sh --worker Minimal-OS-SDK/programs/dlr

$(install2_object): $(install2_mpkg)
	mkdir -p $(dir $@)
	objcopy -I binary -O elf64-x86-64 -B i386 \
		--redefine-sym _binary_build_resources_install2_mpkg_start=_binary_install2_mpkg_start \
		--redefine-sym _binary_build_resources_install2_mpkg_end=_binary_install2_mpkg_end \
		$< $@

$(install3_mpkg): $(mode_stamp) $(mosdesk_install_dep) $(install3_tree_deps) tools/mkpkg/mkpkg.py
	mkdir -p $(dir $@)
	$(MKPKG) $(install3_source) $@ --store

$(install3_object): $(install3_mpkg)
	mkdir -p $(dir $@)
	objcopy -I binary -O elf64-x86-64 -B i386 \
		--redefine-sym _binary_build_resources_install3_mpkg_start=_binary_install3_mpkg_start \
		--redefine-sym _binary_build_resources_install3_mpkg_end=_binary_install3_mpkg_end \
		$< $@

# Install payload: GRUB's boot images for the 'install' command. GRUB loads
# it as the boot module "mosinst" (and the kernel file as "moskernel"). GRUB's BIOS core
# looks for its files on partition 1 of the boot disk; GRUB for UEFI uses
# the partition it was started from (the EFI System Partition).
GRUB_PC_DIR ?= /usr/lib/grub/i386-pc
GRUB_EFI_DIR ?= /usr/lib/grub/x86_64-efi
GRUB_INSTALL_MODULES := part_gpt part_msdos fat normal multiboot2 configfile \
	all_video boot echo test
installer_dir := build/installer
installer_mpkg := targets/x86_64/iso/boot/mosinst.mpkg

.PHONY: installer-payload
installer-payload:
	rm -rf $(installer_dir)
	mkdir -p $(installer_dir)
	# The kernel itself is not in here: GRUB loads boot/kernel.bin a second
	# time as the "moskernel" module, so it is not stored twice.
	cp $(GRUB_PC_DIR)/boot.img $(installer_dir)/boot.img
	grub-mkimage -d $(GRUB_PC_DIR) -O i386-pc -o $(installer_dir)/core.img \
		-p '(,1)/boot/grub' biosdisk $(GRUB_INSTALL_MODULES)
	@if [ -d "$(GRUB_EFI_DIR)" ]; then \
		grub-mkimage -d $(GRUB_EFI_DIR) -O x86_64-efi -o $(installer_dir)/bootx64.efi \
			-p /boot/grub $(GRUB_INSTALL_MODULES); \
	else \
		echo "installer: $(GRUB_EFI_DIR) missing (install grub-efi-amd64-bin);" \
			"installed disks and the ISO will boot with BIOS only" >&2; \
	fi
	$(MKPKG) $(installer_dir) $(installer_mpkg) --store

.PHONY: build-x86_64
build-x86_64: minimaSSL-slib $(source_object_files) $(audio_object_files)
	mkdir -p dist/x86_64
	$(LD) -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(source_object_files) $(audio_object_files)
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	$(MAKE) --no-print-directory installer-payload
	# grub-mkrescue makes a BIOS + UEFI hybrid image when both GRUB
	# platforms are installed (grub-pc-bin, grub-efi-amd64-bin, mtools).
	grub-mkrescue -o dist/x86_64/kernel.iso targets/x86_64/iso

.PHONY: clean
clean:
	rm -rf build dist Minimal-OS-SDK/build
	find . -type f -name '*.run' -delete
	rm -f Minimal-OS-SDK/minimaSSL.slib src/resources/install1/minimaSSL.slib

.PHONY: audio
audio: $(audio_obj_files_build) $(audio_obj_files_src)

src/resources/%.o: src/resources/%.wav
	python3 tools/audioconverter/wavtoheader.py $< $@

# 'run' also attaches an empty second disk (sparse file, takes no space
# until written) for the installer, and gives the virtual GPU 64 MiB VRAM.
INSTALL_DISK ?= install-target.img
INSTALL_DISK_SIZE ?= 8G
QEMU_VGA ?= -vga none -device VGA,vgamem_mb=64
QEMU_INSTALL_DISK = -drive id=disk1,file=$(INSTALL_DISK),if=none,format=raw -device ide-hd,drive=disk1,bus=ahci.1
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

$(INSTALL_DISK):
	truncate -s $(INSTALL_DISK_SIZE) $@

.PHONY: run
run: build-x86_64 $(INSTALL_DISK)
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M $(QEMU_VGA) $(QEMU_INSTALL_DISK) -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log -serial stdio -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1 -device usb-mouse,bus=uhci.0,port=2 -audiodev $(QEMU_AUDIO_DRIVER),id=speaker -machine pcspk-audiodev=speaker  -audiodev $(QEMU_AUDIO_DRIVER),id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0 -netdev $(QEMU_NETDEV) -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: mosdesk-build mosdesk-stage run-mosdesk
# MOSDESK_USE_LOCAL=1 builds the checkouts as they are (no submodule
# fetching), e.g. when MOSdesk and the SDK have unpushed changes.
MOSDESK_USE_LOCAL ?= 0

mosdesk-build:
	mkdir -p $(dir $(MOSDESK_DIR))
	@if [ "$(MOSDESK_USE_LOCAL)" = 1 ]; then exit 0; fi; \
	if ! git submodule update --init --recursive Minimal-OS-SDK; then \
		git -C Minimal-OS-SDK fetch origin && \
		git -C Minimal-OS-SDK checkout --detach FETCH_HEAD; \
	fi
	@if [ "$(MOSDESK_USE_LOCAL)" = 1 ]; then exit 0; fi; \
	if ! git submodule update --init "$(MOSDESK_DIR)"; then \
		echo "mosdesk-build: pinned MOSdesk commit is not on the remote; using its latest default branch" >&2; \
		git submodule update --init --remote --force "$(MOSDESK_DIR)"; \
	fi
	@if [ "$(MOSDESK_USE_LOCAL)" = 1 ]; then exit 0; fi; \
	if ! git -C "$(MOSDESK_DIR)" submodule update --init; then \
		echo "mosdesk-build: a pinned nested commit is not on the remote; using the latest default branches" >&2; \
		git -C "$(MOSDESK_DIR)" submodule update --init --remote --force; \
	fi
	# MOSdesk is built with this checkout's SDK: its apps talk to this
	# kernel, so they must use the same syscall headers.
	CC=$(CC) LD=$(LD) MKPKG_TOOL=$(CURDIR)/tools/mkpkg/mkpkg.py MKPKG_JOBS=$(MKPKG_JOBS) \
		MOSDESK_SDK_DIR=$(CURDIR)/Minimal-OS-SDK $(MOSDESK_DIR)/mosdesk/build.sh

mosdesk-stage: mosdesk-build $(dlr_run) $(dlrconnect_run) $(fglcube_run)
	rm -rf build/resources/mosdesk-install
	mkdir -p build/resources/mosdesk-install/install2/mosWM \
		build/resources/mosdesk-install/install3
	cp -a $(install2_base_source)/. build/resources/mosdesk-install/install2/
	cp -a $(install3_base_source)/. build/resources/mosdesk-install/install3/
	# The MOSdesk package is rooted at 0:/; install2 is unpacked into 0:/programs.
	cp -a $(MOSDESK_DIR)/mosdesk/dist/mosdesk/programs/. build/resources/mosdesk-install/install2/
	cp -a $(MOSDESK_DIR)/mosWM/dist/mosWM/programs/mosWM/. build/resources/mosdesk-install/install2/mosWM/
	cp $(MOSDESK_DIR)/mosWM/dist/mosWM/services/mosWM.service build/resources/mosdesk-install/install3/mosWM.service

run-mosdesk:
	$(MAKE) MOSDESK=1 run

# Boots the disk the installer wrote ($(INSTALL_DISK)) without the CD.
.PHONY: run-installed run-installed-uefi
run-installed:
	qemu-system-x86_64 -smp 2 -m 1024M $(QEMU_VGA) -boot c -serial stdio \
		-device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1 -device usb-mouse,bus=uhci.0,port=2 \
		-device ahci,id=ahci -drive id=disk0,file=$(INSTALL_DISK),if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0 \
		-netdev $(QEMU_NETDEV) -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

# Same with UEFI firmware (needs the ovmf package).
run-installed-uefi:
	cp $(OVMF_VARS) build/ovmf-vars.fd
	qemu-system-x86_64 -machine q35 -smp 2 -m 1024M $(QEMU_VGA) -serial stdio \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) -drive if=pflash,format=raw,file=build/ovmf-vars.fd \
		-drive id=disk0,file=$(INSTALL_DISK),if=none,format=raw -device ide-hd,drive=disk0,bus=ide.0 \
		-netdev $(QEMU_NETDEV) -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: run-two
run-two: build-x86_64
	@test -f sata256.img || { echo "run-two: missing sata256.img" >&2; exit 1; }
	@test -f sata256-2.img || cp sata256.img sata256-2.img
	@echo "run-two: starting two MOS guests on multicast LAN $(QEMU_TWO_MCAST)"
	@echo "run-two: addresses are automatic: 192.168.100.2 and 192.168.100.3"
	@trap 'kill $$qemu1 $$qemu2 2>/dev/null || true; wait $$qemu1 $$qemu2 2>/dev/null || true' INT TERM EXIT; \
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu-one.log -serial tcp:127.0.0.1:5566,server=on,wait=off -usb -device usb-kbd -device usb-mouse -audiodev $(QEMU_AUDIO_DRIVER),id=speaker1 -machine pcspk-audiodev=speaker1 -device ahci,id=ahci1 -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci1.0 -netdev socket,id=net-one,mcast=$(QEMU_TWO_MCAST) -device rtl8139,netdev=net-one,mac=52:54:00:12:34:56 & qemu1=$$!; \
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu-two.log -serial tcp:127.0.0.1:5567,server=on,wait=off -usb -device usb-kbd -device usb-mouse -audiodev $(QEMU_AUDIO_DRIVER),id=speaker2 -machine pcspk-audiodev=speaker2 -device ahci,id=ahci2 -drive id=disk1,file=sata256-2.img,if=none,format=raw -device ide-hd,drive=disk1,bus=ahci2.0 -netdev socket,id=net-two,mcast=$(QEMU_TWO_MCAST) -device rtl8139,netdev=net-two,mac=52:54:00:12:34:57 & qemu2=$$!; \
	wait $$qemu1 $$qemu2

.PHONY: run-audio
run-audio: build-x86_64
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log  -usb -device usb-kbd -device usb-mouse -serial stdio -audiodev wav,id=speaker,path=speaker.wav -machine pcspk-audiodev=speaker -audiodev wav,id=audio0,path=ac97.wav -device AC97,audiodev=audio0,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0 -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: run-sdl
run-sdl: build-x86_64
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev sdl,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd -device usb-mouse -audiodev sdl,id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0 -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: run-alsa
run-alsa: build-x86_64
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev alsa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd -device usb-mouse -audiodev alsa,id=audio0 -device AC97,audiodev=audio0 -device ahci,id=ahci -drive id=disk0,file=sata256.img,if=none,format=raw -device ide-hd,drive=disk0,bus=ahci.0 -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: run-ps2
run-ps2: build-x86_64
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: run-int
run-int: build-x86_64
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -serial stdio -d int -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd -device usb-mouse -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: run-de
run-de: build-x86_64
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log --no-reboot -serial stdio -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -usb -device usb-kbd -device usb-mouse -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: run-dex
run-dex: build-x86_64
	qemu-system-x86_64 -smp 2 -cdrom dist/x86_64/kernel.iso -m 1024M -boot d -d guest_errors,int,cpu_reset,unimp -D qemu.log --no-reboot -serial stdio -S -s -usb -device usb-kbd -device usb-mouse -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device rtl8139,netdev=net0,mac=52:54:00:12:34:56

.PHONY: ci-test
ci-test:
	bash tools/debug/ci_test.sh

.PHONY: test-host
test-host: build/tests/host/gfx_blit_tests build/tests/host/fgpu_raster_tests build/tests/host/fgsl_tests \
		build/tests/host/fgpu_ir_tests build/tests/host/nv_hw_tests
	./build/tests/host/gfx_blit_tests
	./build/tests/host/fgpu_raster_tests
	./build/tests/host/fgsl_tests
	./build/tests/host/fgpu_ir_tests
	./build/tests/host/nv_hw_tests

build/tests/host/fgpu_raster_tests: tests/host/fgpu_raster_tests.c src/modules/GPU/fgpu_raster.c src/modules/GPU/fgpu_shader.c
	mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $(filter %.c,$^) -lm -o $@

# FGSL compiler (SDK) + shader checker/machine + rasterizer, end to end.
build/tests/host/fgsl_tests: tests/host/fgsl_tests.c src/modules/GPU/fgpu_raster.c src/modules/GPU/fgpu_shader.c Minimal-OS-SDK/include/fglsl.h Minimal-OS-SDK/include/fgpu_abi.h
	mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) -idirafter Minimal-OS-SDK/include $(filter %.c,$^) -lm -o $@

build/tests/host/fgpu_ir_tests: tests/host/fgpu_ir_tests.c src/modules/GPU/fgpu_ir.c src/modules/GPU/fgpu_shader.c Minimal-OS-SDK/include/fglsl.h Minimal-OS-SDK/include/fgpu_abi.h $(header_files)
	mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) -idirafter Minimal-OS-SDK/include $(filter %.c,$^) -lm -o $@

build/tests/host/nv_hw_tests: tests/host/nv_hw_tests.c src/modules/GPU/nv_fw.c $(header_files)
	mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $(filter %.c,$^) -o $@

build/tests/host/gfx_blit_tests: tests/host/gfx_blit_tests.c src/impl/handlers/gfx_blit.c
	mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $^ -o $@

.PHONY: ci-interact
ci-interact:
	bash tools/debug/ci_interact.sh

.PHONY: dlr-cli-test
dlr-cli-test:
	bash tools/debug/test_dlr_present.sh
