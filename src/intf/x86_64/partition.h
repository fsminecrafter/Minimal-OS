#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/blockdev.h"

/*
 * MBR and GPT partition tables.
 *
 * partition_scan() reads a disk's table and registers each partition as a
 * child block device (see blockdev.h). The other calls change the table on
 * disk and rescan.
 *
 * MBR addresses at most 2 TiB (32-bit sector numbers); bigger disks need
 * GPT, which partition_create_table() picks for PART_TABLE_AUTO.
 */

typedef enum {
    PART_TABLE_NONE = 0,
    PART_TABLE_MBR,
    PART_TABLE_GPT,
    PART_TABLE_AUTO,        // create only: GPT unless the disk is tiny
} partition_table_t;

typedef enum {
    PART_KIND_MINIMAFS = 0, // MinimaFS volume
    PART_KIND_ESP,          // EFI System Partition (FAT32)
    PART_KIND_BIOS_BOOT,    // GPT BIOS boot partition (holds GRUB's core)
    PART_KIND_FAT,          // ordinary FAT32 data
    PART_KIND_OTHER,
} partition_kind_t;

#define PART_MBR_TYPE_MINIMAFS 0x7F   // "alternative OS development"
#define PART_MBR_TYPE_ESP      0xEF
#define PART_MBR_TYPE_FAT32LBA 0x0C
#define PART_MBR_TYPE_GPT      0xEE

#define PART_ALIGN_BYTES (1024ULL * 1024ULL)   // 1 MiB

partition_table_t partition_table_type(blockdev_t* disk);
const char* partition_table_name(partition_table_t type);

// Registers the partitions found on disk. Returns how many.
uint32_t partition_scan(blockdev_t* disk);

// Writes a new empty table (destroys the old one).
bool partition_create_table(blockdev_t* disk, partition_table_t type);

// Adds a partition. start_lba 0 = first free aligned spot, sectors 0 = all
// remaining space in that gap. Returns the new partition device.
blockdev_t* partition_add(blockdev_t* disk, uint64_t start_lba,
                          uint64_t sectors, partition_kind_t kind,
                          const char* label);

bool partition_delete(blockdev_t* disk, uint32_t number);

partition_kind_t partition_kind_of(const blockdev_t* part);
const char* partition_kind_name(partition_kind_t kind);
bool partition_kind_from_name(const char* name, partition_kind_t* kind);

// "1234abcd-..." text of a GPT GUID as stored on disk.
void partition_guid_to_string(const uint8_t guid[16], char out[37]);

uint32_t partition_crc32(const void* data, uint32_t len);
