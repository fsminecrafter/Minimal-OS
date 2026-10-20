#include <stdbool.h>
#include <stdint.h>
#include "graphics.h"
#include "serial.h"
#include "string.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/blockdev.h"
#include "x86_64/partition.h"
#include "x86_64/minimafs.h"
#include "minimafshandler.h"

/*
 * disks                         list disks, partitions and filesystems
 * part <disk>                   show a disk's partition table
 * part <disk> init [gpt|mbr] --yes
 * part <disk> add <size|rest> [minimafs|esp|fat|biosboot] [label]
 * part <disk> del <number>
 * mount                         list mounted drives
 * mount <device> [drive]        mount a MinimaFS volume as drive N:
 * mount <drive>                 (old form) mount the boot volume as N:
 * umount <drive>
 * drives                        same as plain 'mount'
 */

static void say(const char* s) { graphics_write_textr(s); }

static bool parse_uint(const char* s, uint64_t* out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

// "512M", "20G", "2T", "4096K", plain number = MiB.
static bool parse_size(const char* s, uint64_t* bytes) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    const char* p = s;
    while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
    if (p == s) return false;
    uint64_t mul = 1024ULL * 1024;
    if (*p == 'K' || *p == 'k') mul = 1024ULL;
    else if (*p == 'M' || *p == 'm') mul = 1024ULL * 1024;
    else if (*p == 'G' || *p == 'g') mul = 1024ULL * 1024 * 1024;
    else if (*p == 'T' || *p == 't') mul = 1024ULL * 1024 * 1024 * 1024;
    else if (*p != '\0') return false;
    *bytes = v * mul;
    return true;
}

static void print_device_line(blockdev_t* d) {
    char size[24], line[160];
    blockdev_format_size(blockdev_size_bytes(d), size, sizeof(size));
    if (d->kind == BLOCKDEV_KIND_DISK) {
        partition_table_t t = partition_table_type(d);
        snprintf(line, sizeof(line), "%s  %s  %s  [%s]",
                 d->name, size, d->model[0] ? d->model : "disk",
                 t != PART_TABLE_NONE ? partition_table_name(t) :
                 blockdev_fs_name(d->fs));
    } else {
        snprintf(line, sizeof(line), "  %s  %s  %s  %s%s%s",
                 d->name, size,
                 partition_kind_name(partition_kind_of(d)),
                 blockdev_fs_name(d->fs),
                 d->label[0] ? "  \"" : "",
                 d->label[0] ? d->label : "");
        if (d->label[0]) strcat(line, "\"");
    }
    say(line);
    if (d->mounted_drive >= 0) {
        char m[24];
        snprintf(m, sizeof(m), "  -> %d:", d->mounted_drive);
        say(m);
    }
    say("\n");
}

void cmd_disks(int argc, const char** argv) {
    (void)argc; (void)argv;
    initializeminimafs(0);
    if (!blockdev_first_disk()) {
        say("No disks found.\n");
        return;
    }
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* disk = blockdev_get(i);
        if (!disk || disk->kind != BLOCKDEV_KIND_DISK) continue;
        print_device_line(disk);
        for (uint32_t j = 0; j < BLOCKDEV_MAX; j++) {
            blockdev_t* p = blockdev_get(j);
            if (p && p->parent == disk) print_device_line(p);
        }
    }
}

static void part_usage(void) {
    say("usage: part <disk>\n"
        "       part <disk> init [gpt|mbr] --yes\n"
        "       part <disk> add <size|rest> [minimafs|esp|fat|biosboot] [label]\n"
        "       part <disk> del <number>\n"
        "sizes: 512M, 20G, 2T (plain numbers are MiB)\n");
}

void cmd_part(int argc, const char** argv) {
    if (argc < 2) { part_usage(); return; }
    initializeminimafs(0);
    blockdev_t* disk = blockdev_find(argv[1]);
    if (!disk || disk->kind != BLOCKDEV_KIND_DISK) {
        say("part: not a disk (see 'disks')\n");
        return;
    }

    if (argc == 2) {
        char line[96];
        snprintf(line, sizeof(line), "%s: %s partition table\n", disk->name,
                 partition_table_name(partition_table_type(disk)));
        say(line);
        for (uint32_t j = 0; j < BLOCKDEV_MAX; j++) {
            blockdev_t* p = blockdev_get(j);
            if (!p || p->parent != disk) continue;
            char size[24];
            blockdev_format_size(blockdev_size_bytes(p), size, sizeof(size));
            snprintf(line, sizeof(line), "  %u  start %llu  %s  %s\n",
                     p->part_number, p->start_lba, size,
                     partition_kind_name(partition_kind_of(p)));
            say(line);
        }
        return;
    }

    const char* op = argv[2];
    if (strcmp(op, "init") == 0) {
        partition_table_t type = PART_TABLE_AUTO;
        bool yes = false;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "gpt") == 0) type = PART_TABLE_GPT;
            else if (strcmp(argv[i], "mbr") == 0) type = PART_TABLE_MBR;
            else if (strcmp(argv[i], "--yes") == 0) yes = true;
        }
        if (!yes) {
            say("This erases everything on ");
            say(disk->name);
            say(". Add --yes to continue.\n");
            return;
        }
        for (uint32_t j = 0; j < BLOCKDEV_MAX; j++) {
            blockdev_t* p = blockdev_get(j);
            if (p && (p == disk || p->parent == disk) && p->mounted_drive >= 0) {
                say("part: unmount the disk's drives first (umount N)\n");
                return;
            }
        }
        if (partition_create_table(disk, type)) say("New partition table written.\n");
        else say("part: failed to write the partition table\n");
        return;
    }

    if (strcmp(op, "add") == 0) {
        if (argc < 4) { part_usage(); return; }
        uint64_t sectors = 0;
        if (strcmp(argv[3], "rest") != 0) {
            uint64_t bytes;
            if (!parse_size(argv[3], &bytes) || bytes < disk->sector_size) {
                say("part: bad size\n");
                return;
            }
            sectors = bytes / disk->sector_size;
        }
        partition_kind_t kind = PART_KIND_MINIMAFS;
        const char* label = NULL;
        if (argc >= 5 && !partition_kind_from_name(argv[4], &kind)) {
            say("part: kind must be minimafs, esp, fat or biosboot\n");
            return;
        }
        if (argc >= 6) label = argv[5];
        blockdev_t* p = partition_add(disk, 0, sectors, kind, label);
        if (!p) {
            say("part: no room for that partition (or table full)\n");
            return;
        }
        say("Created ");
        print_device_line(p);
        return;
    }

    if (strcmp(op, "del") == 0 || strcmp(op, "delete") == 0) {
        uint64_t n;
        if (argc < 4 || !parse_uint(argv[3], &n)) { part_usage(); return; }
        if (partition_delete(disk, (uint32_t)n)) say("Partition deleted.\n");
        else say("part: could not delete that partition\n");
        return;
    }
    part_usage();
}

static void print_drives(void) {
    bool any = false;
    for (int n = 0; n < MINIMAFS_MAX_DRIVES; n++) {
        minimafs_drive_t* d = get_drive((uint8_t)n);
        if (!d || !d->mounted) continue;
        any = true;
        blockdev_t* dev = (blockdev_t*)d->device_handle;
        char total[24], used[24], line[160];
        blockdev_format_size(d->storage_desc.total_size, total, sizeof(total));
        blockdev_format_size((uint64_t)(d->storage_desc.total_blocks -
                                        d->storage_desc.free_blocks) * MINIMAFS_BLOCK_SIZE,
                             used, sizeof(used));
        snprintf(line, sizeof(line), "%d:  %s  \"%s\"  %s used of %s\n", n,
                 dev ? dev->name : "?", d->storage_desc.drive_name, used, total);
        say(line);
    }
    if (!any) say("No drives mounted.\n");
}

void cmd_mount(int argc, const char** argv) {
    initializeminimafs(0);
    if (argc < 2) {
        print_drives();
        return;
    }

    blockdev_t* dev = NULL;
    int drive = -1;
    uint64_t v;
    if (parse_uint(argv[1], &v)) {
        // Old form: mount <drive> mounts the boot volume.
        dev = getminimadrive();
        drive = (int)v;
    } else {
        dev = blockdev_find(argv[1]);
        if (argc >= 3) {
            if (!parse_uint(argv[2], &v)) { say("mount: bad drive number\n"); return; }
            drive = (int)v;
        } else {
            drive = storage_free_drive_number(0);
        }
    }
    if (!dev) { say("mount: unknown device (see 'disks')\n"); return; }
    if (drive < 0 || drive >= MINIMAFS_MAX_DRIVES) { say("mount: no free drive number\n"); return; }
    if (dev->fs == BLOCKDEV_FS_PARTITIONED) {
        say("mount: that disk has partitions; mount one of them instead\n");
        return;
    }

    int r = mountdrive(dev, drive);
    char line[96];
    switch (r) {
        case 1:
            snprintf(line, sizeof(line), "%s mounted as %d:\n", dev->name, drive);
            say(line);
            break;
        case 2: say("mount: already mounted\n"); break;
        case 3: say("mount: no MinimaFS volume there (format it first)\n"); break;
        case 4: say("mount: invalid root block\n"); break;
        case 5: say("mount: invalid volume\n"); break;
        default: say("mount: failed\n"); break;
    }
}

void cmd_umount(int argc, const char** argv) {
    uint64_t n;
    if (argc < 2 || !parse_uint(argv[1], &n) || n >= MINIMAFS_MAX_DRIVES) {
        say("usage: umount <drive number>\n");
        return;
    }
    if (n == 0) say("Warning: 0: is the system drive.\n");
    if (minimafs_unmount((uint8_t)n)) say("Unmounted.\n");
    else say("umount: not mounted\n");
}

void cmd_drives(int argc, const char** argv) {
    (void)argc; (void)argv;
    print_drives();
}

void cmd_initdisk(int argc, const char** argv) {
    (void)argc; (void)argv;
    blockdev_t* dev = initializeminimafs(0);
    if (!dev) {
        say("No disk found.\n");
        return;
    }
    say("Boot volume: ");
    say(dev->name);
    say("\n");
}

static void register_storage_commands(void) {
    command_register("disks", cmd_disks);
    command_register("lsblk", cmd_disks);
    command_register("part", cmd_part);
    command_register("mount", cmd_mount);
    command_register("umount", cmd_umount);
    command_register("drives", cmd_drives);
    command_register("initdisk", cmd_initdisk);
}

REGISTER_COMMAND(register_storage_commands);
