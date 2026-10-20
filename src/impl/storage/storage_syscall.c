#include "x86_64/syscall.h"
#include "x86_64/blockdev.h"
#include "x86_64/partition.h"
#include "x86_64/installer.h"
#include "minimafshandler.h"
#include "string.h"

extern bool syscall_user_range_ok(const void* ptr, size_t len, bool write);

static bool disk_in_use(blockdev_t* disk) {
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = blockdev_get(i);
        if (d && (d == disk || d->parent == disk) && d->mounted_drive >= 0) return true;
    }
    return false;
}

static void fill(syscall_storage_device_t* out, blockdev_t* d) {
    memset(out, 0, sizeof(*out));
    strncpy(out->name, d->name, sizeof(out->name) - 1);
    if (d->parent) strncpy(out->parent, d->parent->name, sizeof(out->parent) - 1);
    strncpy(out->model, d->model, sizeof(out->model) - 1);
    strncpy(out->label, d->label, sizeof(out->label) - 1);
    out->kind = d->kind == BLOCKDEV_KIND_DISK ? SYSCALL_STORAGE_KIND_DISK
                                              : SYSCALL_STORAGE_KIND_PARTITION;
    if (d->kind == BLOCKDEV_KIND_DISK) {
        partition_table_t t = partition_table_type(d);
        out->table = t == PART_TABLE_GPT ? SYSCALL_STORAGE_TABLE_GPT :
                     t == PART_TABLE_MBR ? SYSCALL_STORAGE_TABLE_MBR :
                                           SYSCALL_STORAGE_TABLE_NONE;
    } else {
        out->part_kind = (uint8_t)partition_kind_of(d);
    }
    out->fs = (uint8_t)d->fs;
    out->mounted_drive = d->mounted_drive;
    if (d->mounted_drive == 0) out->flags |= SYSCALL_STORAGE_FLAG_BOOT;
    blockdev_t* disk = d;
    while (disk->parent) disk = disk->parent;
    if (disk_in_use(disk)) out->flags |= SYSCALL_STORAGE_FLAG_IN_USE;
    for (uint32_t i = 0; i < BLOCKDEV_MAX && d->kind == BLOCKDEV_KIND_DISK; i++) {
        blockdev_t* p = blockdev_get(i);
        if (p && p->parent == d && p->mounted_drive == 0) out->flags |= SYSCALL_STORAGE_FLAG_BOOT;
    }
    out->sector_size = d->sector_size;
    out->size_bytes = blockdev_size_bytes(d);
}

uint64_t storage_syscall(syscall_storage_request_t* r) {
    if (!r || !syscall_user_range_ok(r, sizeof(*r), true) || r->size != sizeof(*r) ||
        r->version != SYS_STORAGE_ABI_VERSION) return SYS_ERR_INVAL;
    switch (r->op) {
        case SYS_STORAGE_LIST: {
            if (r->capacity && (!r->devices ||
                !syscall_user_range_ok(r->devices,
                                       (size_t)r->capacity * sizeof(syscall_storage_device_t),
                                       true))) return SYS_ERR_INVAL;
            uint32_t n = 0;
            // Disks first, each followed by its partitions.
            for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
                blockdev_t* disk = blockdev_get(i);
                if (!disk || disk->kind != BLOCKDEV_KIND_DISK) continue;
                if (n < r->capacity) fill(&r->devices[n], disk);
                n++;
                for (uint32_t j = 0; j < BLOCKDEV_MAX; j++) {
                    blockdev_t* p = blockdev_get(j);
                    if (!p || p->parent != disk) continue;
                    if (n < r->capacity) fill(&r->devices[n], p);
                    n++;
                }
            }
            return n;
        }
        case SYS_STORAGE_STATUS: {
            r->install_payload = installer_payload_available() ? 1u : 0u;
            memset(r->boot_device, 0, sizeof(r->boot_device));
            blockdev_t* boot = getminimadrive();
            if (boot && boot->mounted_drive == 0)
                strncpy(r->boot_device, boot->name, sizeof(r->boot_device) - 1);
            return SYS_SUCCESS;
        }
        default:
            return SYS_ERR_INVAL;
    }
}
