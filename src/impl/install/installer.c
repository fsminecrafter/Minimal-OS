#include "x86_64/installer.h"
#include "x86_64/blockdev.h"
#include "x86_64/partition.h"
#include "x86_64/fat32.h"
#include "x86_64/memlayout.h"
#include "x86_64/minimafs.h"
#include "x86_64/pkgformat.h"
#include "x86_64/lzss.h"
#include "x86_64/allocator.h"
#include "x86_64/random.h"
#include "minimafshandler.h"
#include "serial.h"
#include "string.h"

#define ESP_BYTES       (256ULL * 1024 * 1024)   // kernel (tens of MiB with MOSdesk) + GRUB
#define BIOS_BOOT_BYTES (1024ULL * 1024)
#define MIN_SYSTEM_BYTES (64ULL * 1024 * 1024)

/* GRUB i386-pc boot sector layout (grub/i386/pc/boot.h). */
#define GRUB_BOOT_BPB_START      0x03
#define GRUB_BOOT_BPB_END        0x5A
#define GRUB_BOOT_KERNEL_SECTOR  0x5C
#define GRUB_BOOT_BOOT_DRIVE     0x64
#define GRUB_BOOT_DRIVE_CHECK    0x66
#define GRUB_BOOT_NT_MAGIC       0x1B8
#define GRUB_KERNEL_SEGMENT      0x820   // where diskboot loads the rest

/* ================================================================
 * Payload
 * ================================================================ */

typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint8_t* owned;             // set when decompressed
} payload_file_t;

static bool payload_module(const uint8_t** data, uint32_t* size) {
    memlayout_module_t m;
    if (!memlayout_find_module(kernel_multiboot_info, "mosinst", &m)) return false;
    if (m.end <= m.start || m.end - m.start < MPKG_HEADER_SIZE) return false;
    *data = (const uint8_t*)(uintptr_t)m.start;
    *size = (uint32_t)(m.end - m.start);
    return memcmp(*data, MPKG_MAGIC, MPKG_MAGIC_SIZE) == 0;
}

static bool kernel_image(payload_file_t* out);
static void payload_release(payload_file_t* f);

bool installer_payload_available(void) {
    const uint8_t* d;
    uint32_t s;
    if (!payload_module(&d, &s)) return false;
    payload_file_t k;
    bool ok = kernel_image(&k);
    if (ok) payload_release(&k);
    return ok;
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool payload_get(const char* name, payload_file_t* out) {
    memset(out, 0, sizeof(*out));
    const uint8_t* pkg;
    uint32_t size;
    if (!payload_module(&pkg, &size)) return false;
    uint32_t count = rd32(pkg + 12);
    if ((uint64_t)MPKG_HEADER_SIZE + (uint64_t)count * MPKG_ENTRY_SIZE > size) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = pkg + MPKG_HEADER_SIZE + i * MPKG_ENTRY_SIZE;
        char entry_name[MPKG_NAME_SIZE];
        memcpy(entry_name, e, MPKG_NAME_SIZE);
        entry_name[MPKG_NAME_SIZE - 1] = '\0';
        if (strcmp(entry_name, name) != 0) continue;
        uint32_t method = rd32(e + MPKG_NAME_SIZE + 4);
        uint32_t raw = rd32(e + MPKG_NAME_SIZE + 8);
        uint32_t packed = rd32(e + MPKG_NAME_SIZE + 12);
        uint32_t offset = rd32(e + MPKG_NAME_SIZE + 16);
        if ((uint64_t)offset + packed > size) return false;
        if (method == MPKG_METHOD_STORE) {
            out->data = pkg + offset;
            out->size = packed;
            return true;
        }
        if (method == MPKG_METHOD_LZSS) {
            out->owned = (uint8_t*)alloc_unzeroed(raw ? raw : 1);
            if (!out->owned) return false;
            if (lzss_decompress(pkg + offset, packed, out->owned, raw) != raw) {
                free_mem(out->owned);
                out->owned = NULL;
                return false;
            }
            out->data = out->owned;
            out->size = raw;
            return true;
        }
        return false;
    }
    return false;
}

// The running kernel's file: GRUB's "moskernel" module, or (older
// payloads) a kernel.bin inside the payload.
static bool kernel_image(payload_file_t* out) {
    memset(out, 0, sizeof(*out));
    memlayout_module_t m;
    if (memlayout_find_module(kernel_multiboot_info, "moskernel", &m) && m.end > m.start) {
        out->data = (const uint8_t*)(uintptr_t)m.start;
        out->size = (uint32_t)(m.end - m.start);
        return true;
    }
    return payload_get("kernel.bin", out);
}

static void payload_release(payload_file_t* f) {
    if (f->owned) free_mem(f->owned);
    memset(f, 0, sizeof(*f));
}

/* ================================================================
 * Helpers
 * ================================================================ */

static void fail(char* err, size_t err_size, const char* msg) {
    serial_write_str("install: ");
    serial_write_str(msg);
    serial_write_str("\n");
    if (err && err_size) {
        strncpy(err, msg, err_size - 1);
        err[err_size - 1] = '\0';
    }
}

static void step(storage_progress_fn progress, uint32_t percent, const char* text) {
    serial_write_str("install: ");
    serial_write_str(text);
    serial_write_str("\n");
    if (progress) progress(percent, text);
}

static blockdev_t* disk_of(blockdev_t* dev) {
    while (dev && dev->kind == BLOCKDEV_KIND_PARTITION) dev = dev->parent;
    return dev;
}

static bool disk_has_mounts(blockdev_t* disk) {
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = blockdev_get(i);
        if (d && (d == disk || d->parent == disk) && d->mounted_drive >= 0) return true;
    }
    return false;
}

static blockdev_t* find_partition_of_kind(blockdev_t* disk, partition_kind_t kind) {
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = blockdev_get(i);
        if (d && d->parent == disk && partition_kind_of(d) == kind) return d;
    }
    return NULL;
}

/* ================================================================
 * BIOS boot code (GRUB boot.img + core.img)
 * ================================================================ */

// core_lba is absolute on the disk. core.img starts with diskboot.img,
// whose last 12 bytes are a block list telling it where the rest is.
static bool write_bios_boot(blockdev_t* disk, uint64_t core_lba, uint64_t room_sectors,
                            char* err, size_t err_size) {
    if (disk->sector_size != 512) {
        fail(err, err_size, "BIOS boot needs 512-byte sectors; disk will boot with UEFI only");
        return false;
    }
    payload_file_t boot, core;
    if (!payload_get("boot.img", &boot) || boot.size != 512 ||
        !payload_get("core.img", &core) || core.size < 1024) {
        fail(err, err_size, "install payload has no GRUB BIOS images");
        payload_release(&boot);
        return false;
    }
    uint32_t core_sectors = (core.size + 511) / 512;
    if (core_sectors > room_sectors) {
        fail(err, err_size, "no room for GRUB's core.img");
        payload_release(&boot);
        payload_release(&core);
        return false;
    }

    uint8_t* buf = (uint8_t*)alloc((uint64_t)core_sectors * 512);
    uint8_t* mbr = (uint8_t*)alloc(512);
    bool ok = buf && mbr;
    if (ok) {
        memcpy(buf, core.data, core.size);
        uint8_t* list = buf + 512 - 12;
        uint64_t start = core_lba + 1;
        memcpy(list, &start, 8);
        uint16_t len = (uint16_t)(core_sectors - 1);
        memcpy(list + 8, &len, 2);
        uint16_t seg = GRUB_KERNEL_SEGMENT;
        memcpy(list + 10, &seg, 2);
        ok = blockdev_write(disk, core_lba, core_sectors, buf);
    }
    if (ok) ok = blockdev_read(disk, 0, 1, mbr);
    if (ok) {
        // Boot code from boot.img; keep the disk's BPB area, disk
        // signature and partition table.
        uint8_t keep_bpb[GRUB_BOOT_BPB_END - GRUB_BOOT_BPB_START];
        memcpy(keep_bpb, mbr + GRUB_BOOT_BPB_START, sizeof(keep_bpb));
        memcpy(mbr, boot.data, GRUB_BOOT_NT_MAGIC);
        memcpy(mbr + GRUB_BOOT_BPB_START, keep_bpb, sizeof(keep_bpb));
        memcpy(mbr + GRUB_BOOT_KERNEL_SECTOR, &core_lba, 8);
        mbr[GRUB_BOOT_BOOT_DRIVE] = 0xFF;          // use the drive the BIOS booted
        mbr[GRUB_BOOT_DRIVE_CHECK] = 0x90;         // some BIOSes pass a bad drive number
        mbr[GRUB_BOOT_DRIVE_CHECK + 1] = 0x90;
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        ok = blockdev_write(disk, 0, 1, mbr);
    }
    if (buf) free_mem(buf);
    if (mbr) free_mem(mbr);
    payload_release(&boot);
    payload_release(&core);
    if (!ok) fail(err, err_size, "writing the BIOS boot code failed");
    return ok;
}

/* ================================================================
 * Boot partition (FAT32)
 * ================================================================ */

static bool write_boot_partition(blockdev_t* esp, const char* root_id,
                                 char* err, size_t err_size) {
    payload_file_t kernel, efi;
    if (!kernel_image(&kernel)) {
        fail(err, err_size, "the kernel image was not loaded (boot entry lacks 'moskernel')");
        return false;
    }
    bool have_efi = payload_get("bootx64.efi", &efi);
    if (!have_efi) serial_write_str("install: payload has no UEFI loader, BIOS boot only\n");

    const uint8_t* pkg;
    uint32_t pkg_size;
    payload_module(&pkg, &pkg_size);

    char* cfg = (char*)alloc(2048);
    if (!cfg) {
        payload_release(&kernel);
        if (have_efi) payload_release(&efi);
        fail(err, err_size, "out of memory");
        return false;
    }
    snprintf(cfg, 2048,
             "# Written by the Minimal-OS installer.\n"
             "set timeout=3\n"
             "set default=0\n"
             "insmod all_video\n"
             "\n"
             "menuentry \"Minimal-OS\" {\n"
             "\tmultiboot2 /boot/kernel.bin root=%s\n"
             "\tmodule2 /boot/kernel.bin moskernel\n"
             "\tmodule2 /boot/mosinst.mpk mosinst\n"
             "\tboot\n"
             "}\n"
             "\n"
             "menuentry \"Minimal-OS (Debug)\" {\n"
             "\tmultiboot2 /boot/kernel.bin root=%s --debug\n"
             "\tmodule2 /boot/kernel.bin moskernel\n"
             "\tmodule2 /boot/mosinst.mpk mosinst\n"
             "\tboot\n"
             "}\n"
             "\n"
             "menuentry \"Minimal-OS (No Disk)\" {\n"
             "\tmultiboot2 /boot/kernel.bin no-disk\n"
             "\tmodule2 /boot/kernel.bin moskernel\n"
             "\tmodule2 /boot/mosinst.mpk mosinst\n"
             "\tboot\n"
             "}\n",
             root_id, root_id);

    bool ok = false;
    fat32_builder_t* fs = fat32_create(esp, "MOSBOOT", (uint32_t)random_u64());
    if (fs) {
        uint32_t root = fat32_root(fs);
        uint32_t efi_dir = fat32_mkdir(fs, root, "EFI");
        uint32_t efi_boot = efi_dir ? fat32_mkdir(fs, efi_dir, "BOOT") : 0;
        uint32_t boot = fat32_mkdir(fs, root, "BOOT");
        uint32_t grub = boot ? fat32_mkdir(fs, boot, "GRUB") : 0;
        ok = efi_boot && grub;
        if (ok && have_efi) ok = fat32_add_file(fs, efi_boot, "BOOTX64.EFI", efi.data, efi.size);
        if (ok) ok = fat32_add_file(fs, boot, "KERNEL.BIN", kernel.data, kernel.size);
        if (ok) ok = fat32_add_file(fs, boot, "MOSINST.MPK", pkg, pkg_size);
        if (ok) ok = fat32_add_file(fs, grub, "GRUB.CFG", cfg, (uint32_t)strlen(cfg));
        ok = fat32_finish(fs) && ok;
    }
    free_mem(cfg);
    payload_release(&kernel);
    if (have_efi) payload_release(&efi);
    if (!ok) fail(err, err_size, "writing the FAT32 boot partition failed");
    esp->fs = ok ? BLOCKDEV_FS_FAT : BLOCKDEV_FS_UNKNOWN;
    return ok;
}

/* ================================================================
 * Install
 * ================================================================ */

static void root_id_of(blockdev_t* system, char out[40]) {
    partition_guid_to_string(system->part_guid, out);
}

bool installer_run(const install_options_t* opt, storage_progress_fn progress,
                   char* err, size_t err_size) {
    if (err && err_size) err[0] = '\0';
    if (!opt) return false;
    if (!installer_payload_available()) {
        fail(err, err_size, "no install payload; boot from the Minimal-OS ISO or an installed system");
        return false;
    }

    step(progress, 2, "Scanning disks");
    initializeminimafs(0);

    blockdev_t* disk = NULL;
    blockdev_t* esp = NULL;
    blockdev_t* system = NULL;
    blockdev_t* bios_boot = NULL;
    bool mbr = false;

    if (opt->disk) {
        disk = blockdev_find(opt->disk);
        if (!disk || disk->kind != BLOCKDEV_KIND_DISK) {
            fail(err, err_size, "target is not a disk");
            return false;
        }
        if (disk_has_mounts(disk)) {
            fail(err, err_size, "the target disk is in use (a drive on it is mounted)");
            return false;
        }
        if (blockdev_size_bytes(disk) < INSTALLER_MIN_DISK_BYTES) {
            fail(err, err_size, "disk is too small (needs 512 MiB)");
            return false;
        }
        mbr = opt->use_mbr;
        if (mbr && disk->sectors > 0xFFFFFFFFull) {
            fail(err, err_size, "disk is bigger than 2 TiB; MBR cannot address it, use GPT");
            return false;
        }

        step(progress, 5, "Writing partition table");
        if (!partition_create_table(disk, mbr ? PART_TABLE_MBR : PART_TABLE_GPT)) {
            fail(err, err_size, "could not write the partition table");
            return false;
        }
        uint64_t esp_bytes = ESP_BYTES;
        esp = partition_add(disk, 0, esp_bytes / disk->sector_size, PART_KIND_ESP, "MOSBOOT");
        if (!esp) {
            fail(err, err_size, "could not create the boot partition");
            return false;
        }
        if (!mbr) {
            bios_boot = partition_add(disk, 0, BIOS_BOOT_BYTES / disk->sector_size,
                                      PART_KIND_BIOS_BOOT, "BIOS boot");
            if (!bios_boot) {
                fail(err, err_size, "could not create the BIOS boot partition");
                return false;
            }
        }
        uint64_t sys_sectors = opt->system_size / disk->sector_size;
        if (opt->system_size && opt->system_size < MIN_SYSTEM_BYTES) {
            fail(err, err_size, "system partition must be at least 64 MiB");
            return false;
        }
        system = partition_add(disk, 0, sys_sectors, PART_KIND_MINIMAFS,
                               opt->label ? opt->label : "system");
        if (!system) {
            fail(err, err_size, "not enough space for the system partition");
            return false;
        }
        // partition_add rescans, which may move devices around; look the
        // boot partitions up again by number.
        esp = find_partition_of_kind(disk, PART_KIND_ESP);
        if (!mbr) bios_boot = find_partition_of_kind(disk, PART_KIND_BIOS_BOOT);
    } else {
        if (!opt->system_part || !opt->boot_part) {
            fail(err, err_size, "need a disk, or both a system and a boot partition");
            return false;
        }
        system = blockdev_find(opt->system_part);
        esp = blockdev_find(opt->boot_part);
        if (!system || !esp || system->kind != BLOCKDEV_KIND_PARTITION ||
            esp->kind != BLOCKDEV_KIND_PARTITION) {
            fail(err, err_size, "system and boot must both be partitions");
            return false;
        }
        if (system == esp) {
            fail(err, err_size, "system and boot must be different partitions");
            return false;
        }
        if (system->mounted_drive >= 0 || esp->mounted_drive >= 0) {
            fail(err, err_size, "unmount the target partitions first");
            return false;
        }
        if (blockdev_size_bytes(system) < MIN_SYSTEM_BYTES) {
            fail(err, err_size, "system partition must be at least 64 MiB");
            return false;
        }
        disk = disk_of(esp);
        mbr = partition_table_type(disk) == PART_TABLE_MBR;
        if (!mbr) bios_boot = find_partition_of_kind(disk, PART_KIND_BIOS_BOOT);
    }

    if (!esp || !system) {
        fail(err, err_size, "partition setup failed");
        return false;
    }

    char root_id[40];
    root_id_of(system, root_id);

    step(progress, 15, "Writing boot partition (GRUB, kernel)");
    if (!write_boot_partition(esp, root_id, err, err_size)) return false;

    step(progress, 35, "Installing BIOS boot code");
    bool bios_ok = false;
    char bios_err[96] = "";
    if (bios_boot) {
        uint64_t abs = bios_boot->start_lba;
        bios_ok = write_bios_boot(disk, abs, bios_boot->sectors, bios_err, sizeof(bios_err));
    } else if (mbr) {
        // Gap between the MBR and the first partition.
        uint64_t first = disk->sectors;
        for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
            blockdev_t* p = blockdev_get(i);
            if (p && p->parent == disk && p->start_lba < first) first = p->start_lba;
        }
        bios_ok = write_bios_boot(disk, 1, first > 1 ? first - 1 : 0, bios_err, sizeof(bios_err));
    } else {
        strncpy(bios_err, "no BIOS boot partition; disk will boot with UEFI only",
                sizeof(bios_err) - 1);
    }
    if (!bios_ok) {
        serial_write_str("install: BIOS boot skipped: ");
        serial_write_str(bios_err);
        serial_write_str("\n");
    }

    step(progress, 45, "Formatting system partition");
    int drive = storage_free_drive_number(1);
    if (drive < 0) {
        fail(err, err_size, "no free drive number");
        return false;
    }
    if (!storage_format_volume(system, (uint8_t)drive,
                               opt->label ? opt->label : "system")) {
        fail(err, err_size, "formatting the system partition failed");
        return false;
    }

    step(progress, 60, "Copying system files");
    bool populated = storage_populate_system((uint8_t)drive, progress);
    minimafs_unmount((uint8_t)drive);
    if (!populated) {
        fail(err, err_size, "copying the system files failed");
        return false;
    }

    step(progress, 100, bios_ok ? "Done. Remove the install media and reboot."
                                 : "Done (UEFI boot only). Remove the install media and reboot.");
    if (!bios_ok && err && err_size) {
        strncpy(err, bios_err, err_size - 1);   // a warning, not a failure
        err[err_size - 1] = '\0';
    }
    return true;
}
