#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "x86_64/storage_setup.h"

/*
 * Installs Minimal-OS onto a disk so it boots without the CD/USB stick.
 *
 * The result boots on both BIOS and UEFI PCs:
 *
 *   GPT disk (default)                 MBR disk (--mbr, disks <= 2 TiB)
 *   p1  EFI System Partition, FAT32    p1  boot partition, FAT32 (active)
 *   p2  BIOS boot (GRUB core.img)      p2  MinimaFS system (0:)
 *   p3  MinimaFS system (0:)           GRUB core.img sits in the gap
 *                                      before p1
 *
 * The FAT32 partition holds GRUB for UEFI (EFI/BOOT/BOOTX64.EFI), the
 * GRUB menu (boot/grub/grub.cfg), the kernel (boot/kernel.bin) and the
 * install payload (boot/mosinst.mpk, 8.3 name) so an installed system can install
 * again. The MinimaFS partition gets the same system files 'format' puts
 * on a disk. grub.cfg passes root=<partition GUID> so the kernel mounts
 * the right volume as 0:.
 *
 * The kernel, GRUB's boot images and GRUB for UEFI come from the install
 * payload: an .mpkg that GRUB loads as the boot module "mosinst" (see
 * targets/x86_64/iso/boot/grub/grub.cfg).
 */

typedef struct {
    // Whole-disk install: the disk is repartitioned.
    const char* disk;
    bool use_mbr;
    uint64_t system_size;       // bytes for the MinimaFS partition, 0 = rest

    // Install into existing partitions instead (disk == NULL).
    const char* system_part;    // becomes MinimaFS
    const char* boot_part;      // becomes the FAT32 boot partition

    const char* label;          // MinimaFS volume name, default "system"
} install_options_t;

// True when GRUB loaded the install payload module.
bool installer_payload_available(void);

// Runs the whole install. On failure err holds a one-line reason.
bool installer_run(const install_options_t* options,
                   storage_progress_fn progress, char* err, size_t err_size);

// Smallest disk the installer accepts, in bytes.
#define INSTALLER_MIN_DISK_BYTES (256ULL * 1024 * 1024)
