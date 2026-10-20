#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Block devices: one interface for every disk and every partition.
 *
 * A disk driver (AHCI, NVMe, ...) registers each drive it finds with
 * blockdev_register_disk(). partition_scan() then adds one child device
 * per partition. A partition only adds an LBA offset and a length to its
 * parent, so the filesystem never needs to know which kind of device it
 * sits on.
 *
 * Names: disks are "sata0", "sata1", "nvme0", ... and their partitions add
 * "p<n>" ("sata0p1", "nvme0p2"), like Linux.
 */

#define BLOCKDEV_MAX          48
#define BLOCKDEV_NAME_SIZE    16
#define BLOCKDEV_MODEL_SIZE   41
#define BLOCKDEV_LABEL_SIZE   37

typedef enum {
    BLOCKDEV_KIND_DISK = 0,
    BLOCKDEV_KIND_PARTITION = 1,
} blockdev_kind_t;

typedef enum {
    BLOCKDEV_FS_UNKNOWN = 0,
    BLOCKDEV_FS_EMPTY,          // first block is all zero
    BLOCKDEV_FS_MINIMAFS,
    BLOCKDEV_FS_FAT,
    BLOCKDEV_FS_PARTITIONED,    // whole disk carrying an MBR/GPT
} blockdev_fs_t;

typedef struct blockdev blockdev_t;

typedef bool (*blockdev_io_fn)(void* ctx, uint64_t lba, uint32_t count,
                               void* buffer);

struct blockdev {
    bool used;
    blockdev_kind_t kind;
    char name[BLOCKDEV_NAME_SIZE];
    char model[BLOCKDEV_MODEL_SIZE];
    uint32_t sector_size;
    uint64_t sectors;             // size in sectors

    // Disks: driver callbacks. Partitions: unused (they go to the parent).
    void* ctx;
    blockdev_io_fn read;
    blockdev_io_fn write;
    uint32_t max_transfer;        // sectors per driver call, 0 = no limit

    // Partitions only.
    blockdev_t* parent;
    uint64_t start_lba;           // in the parent
    uint32_t part_number;         // 1-based, as in the table
    uint8_t mbr_type;             // MBR type byte, 0 on GPT
    uint8_t type_guid[16];        // GPT type, zero on MBR
    uint8_t part_guid[16];        // GPT unique id, zero on MBR
    char label[BLOCKDEV_LABEL_SIZE];

    // Filled in by blockdev_identify().
    blockdev_fs_t fs;
    int mounted_drive;            // MinimaFS drive number, -1 if not mounted
};

void blockdev_init(void);

// Driver side. Returns the new device or NULL when the table is full.
blockdev_t* blockdev_register_disk(const char* prefix, const char* model,
                                   uint32_t sector_size, uint64_t sectors,
                                   void* ctx, blockdev_io_fn read,
                                   blockdev_io_fn write, uint32_t max_transfer);
blockdev_t* blockdev_add_partition(blockdev_t* disk, uint32_t number,
                                   uint64_t start_lba, uint64_t sectors);
// Removes every partition child of disk (before a rescan).
void blockdev_remove_partitions(blockdev_t* disk);

// I/O. lba/count are in the device's own sectors and are bounds checked.
bool blockdev_read(blockdev_t* dev, uint64_t lba, uint32_t count, void* buffer);
bool blockdev_write(blockdev_t* dev, uint64_t lba, uint32_t count,
                    const void* buffer);

// Lookup.
uint32_t blockdev_count(void);
blockdev_t* blockdev_get(uint32_t index);    // index into the table, may skip
blockdev_t* blockdev_find(const char* name);
blockdev_t* blockdev_first_disk(void);
uint64_t blockdev_size_bytes(const blockdev_t* dev);

// Looks at the first sectors and sets dev->fs.
blockdev_fs_t blockdev_identify(blockdev_t* dev);
const char* blockdev_fs_name(blockdev_fs_t fs);

// Finds every disk on every supported controller (AHCI, NVMe) once, scans
// their partition tables and identifies filesystems. Later calls rescan
// partitions only. Returns the number of disks.
uint32_t blockdev_probe_all(void);

// Formats a size like "8.0 TiB" / "512 MiB".
void blockdev_format_size(uint64_t bytes, char* out, size_t out_size);
