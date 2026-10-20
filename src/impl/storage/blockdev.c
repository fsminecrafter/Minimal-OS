#include "x86_64/blockdev.h"
#include "x86_64/partition.h"
#include "x86_64/ahci.h"
#include "x86_64/allocator.h"
#include "x86_64/spinlock.h"
#include "serial.h"
#include "string.h"

static blockdev_t g_devices[BLOCKDEV_MAX];
static spinlock_t g_table_lock = SPINLOCK_INIT;
static bool g_probed;

void blockdev_init(void) {
    memset(g_devices, 0, sizeof(g_devices));
}

static blockdev_t* blockdev_alloc(void) {
    uint64_t flags = spinlock_acquire(&g_table_lock);
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        if (!g_devices[i].used) {
            memset(&g_devices[i], 0, sizeof(g_devices[i]));
            g_devices[i].used = true;
            g_devices[i].mounted_drive = -1;
            spinlock_release(&g_table_lock, flags);
            return &g_devices[i];
        }
    }
    spinlock_release(&g_table_lock, flags);
    return NULL;
}

blockdev_t* blockdev_register_disk(const char* prefix, const char* model,
                                   uint32_t sector_size, uint64_t sectors,
                                   void* ctx, blockdev_io_fn read,
                                   blockdev_io_fn write, uint32_t max_transfer) {
    if (!prefix || !read || sector_size == 0) return NULL;

    // Next free number for this prefix: sata0, sata1, ...
    uint32_t number = 0;
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = &g_devices[i];
        if (!d->used || d->kind != BLOCKDEV_KIND_DISK) continue;
        size_t plen = strlen(prefix);
        if (strncmp(d->name, prefix, plen) == 0) number++;
    }

    blockdev_t* dev = blockdev_alloc();
    if (!dev) return NULL;
    dev->kind = BLOCKDEV_KIND_DISK;
    snprintf(dev->name, sizeof(dev->name), "%s%u", prefix, number);
    if (model) {
        strncpy(dev->model, model, sizeof(dev->model) - 1);
    }
    dev->sector_size = sector_size;
    dev->sectors = sectors;
    dev->ctx = ctx;
    dev->read = read;
    dev->write = write;
    dev->max_transfer = max_transfer;

    serial_write_str("blockdev: ");
    serial_write_str(dev->name);
    serial_write_str(" registered, sectors=");
    serial_write_dec(sectors);
    serial_write_str("\n");
    return dev;
}

blockdev_t* blockdev_add_partition(blockdev_t* disk, uint32_t number,
                                   uint64_t start_lba, uint64_t sectors) {
    if (!disk || disk->kind != BLOCKDEV_KIND_DISK || sectors == 0) return NULL;
    if (start_lba >= disk->sectors || sectors > disk->sectors - start_lba) {
        serial_write_str("blockdev: partition outside disk ignored\n");
        return NULL;
    }
    // A mounted partition survives a rescan; hand back the same device.
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = &g_devices[i];
        if (d->used && d->parent == disk && d->start_lba == start_lba &&
            d->sectors == sectors) {
            d->part_number = number;
            snprintf(d->name, sizeof(d->name), "%sp%u", disk->name, number);
            return d;
        }
    }
    blockdev_t* dev = blockdev_alloc();
    if (!dev) return NULL;
    dev->kind = BLOCKDEV_KIND_PARTITION;
    snprintf(dev->name, sizeof(dev->name), "%sp%u", disk->name, number);
    strncpy(dev->model, disk->model, sizeof(dev->model) - 1);
    dev->sector_size = disk->sector_size;
    dev->sectors = sectors;
    dev->parent = disk;
    dev->start_lba = start_lba;
    dev->part_number = number;
    return dev;
}

void blockdev_remove_partitions(blockdev_t* disk) {
    if (!disk) return;
    uint64_t flags = spinlock_acquire(&g_table_lock);
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = &g_devices[i];
        // A mounted partition keeps its slot so the filesystem's pointer
        // stays valid; the rescan then finds it again by start LBA.
        if (d->used && d->parent == disk && d->mounted_drive < 0) {
            d->used = false;
        }
    }
    spinlock_release(&g_table_lock, flags);
}

static bool blockdev_io(blockdev_t* dev, uint64_t lba, uint32_t count,
                        void* buffer, bool write) {
    if (!dev || !dev->used || !buffer) return false;
    if (count == 0) return true;
    if (lba >= dev->sectors || count > dev->sectors - lba) {
        serial_write_str("blockdev: I/O beyond end of ");
        serial_write_str(dev->name);
        serial_write_str("\n");
        return false;
    }

    blockdev_t* disk = dev;
    while (disk->kind == BLOCKDEV_KIND_PARTITION) {
        lba += disk->start_lba;
        disk = disk->parent;
        if (!disk || !disk->used) return false;
    }

    blockdev_io_fn fn = write ? disk->write : disk->read;
    if (!fn) return false;

    uint8_t* p = (uint8_t*)buffer;
    uint32_t chunk_max = disk->max_transfer ? disk->max_transfer : count;
    while (count > 0) {
        uint32_t chunk = count < chunk_max ? count : chunk_max;
        if (!fn(disk->ctx, lba, chunk, p)) return false;
        lba += chunk;
        count -= chunk;
        p += (uint64_t)chunk * disk->sector_size;
    }
    return true;
}

bool blockdev_read(blockdev_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    return blockdev_io(dev, lba, count, buffer, false);
}

bool blockdev_write(blockdev_t* dev, uint64_t lba, uint32_t count,
                    const void* buffer) {
    return blockdev_io(dev, lba, count, (void*)buffer, true);
}

uint32_t blockdev_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) if (g_devices[i].used) n++;
    return n;
}

blockdev_t* blockdev_get(uint32_t index) {
    if (index >= BLOCKDEV_MAX || !g_devices[index].used) return NULL;
    return &g_devices[index];
}

blockdev_t* blockdev_find(const char* name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        if (g_devices[i].used && strcmp(g_devices[i].name, name) == 0) {
            return &g_devices[i];
        }
    }
    return NULL;
}

blockdev_t* blockdev_first_disk(void) {
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        if (g_devices[i].used && g_devices[i].kind == BLOCKDEV_KIND_DISK) {
            return &g_devices[i];
        }
    }
    return NULL;
}

uint64_t blockdev_size_bytes(const blockdev_t* dev) {
    return dev ? dev->sectors * (uint64_t)dev->sector_size : 0;
}

const char* blockdev_fs_name(blockdev_fs_t fs) {
    switch (fs) {
        case BLOCKDEV_FS_EMPTY: return "empty";
        case BLOCKDEV_FS_MINIMAFS: return "MinimaFS";
        case BLOCKDEV_FS_FAT: return "FAT";
        case BLOCKDEV_FS_PARTITIONED: return "partitioned";
        default: return "unknown";
    }
}

blockdev_fs_t blockdev_identify(blockdev_t* dev) {
    if (!dev) return BLOCKDEV_FS_UNKNOWN;
    uint32_t bytes = 4096;
    if (blockdev_size_bytes(dev) < bytes) {
        dev->fs = BLOCKDEV_FS_UNKNOWN;
        return dev->fs;
    }
    uint8_t* buf = (uint8_t*)alloc(bytes);
    if (!buf) return BLOCKDEV_FS_UNKNOWN;
    dev->fs = BLOCKDEV_FS_UNKNOWN;
    if (blockdev_read(dev, 0, bytes / dev->sector_size, buf)) {
        bool zero = true;
        for (uint32_t i = 0; i < 512; i++) if (buf[i]) { zero = false; break; }
        if (memcmp(buf, "@MAGIC:", 7) == 0) {
            dev->fs = BLOCKDEV_FS_MINIMAFS;
        } else if (zero) {
            dev->fs = BLOCKDEV_FS_EMPTY;
        } else if (buf[510] == 0x55 && buf[511] == 0xAA &&
                   (memcmp(buf + 0x52, "FAT32", 5) == 0 ||
                    memcmp(buf + 0x36, "FAT", 3) == 0)) {
            dev->fs = BLOCKDEV_FS_FAT;
        } else if (dev->kind == BLOCKDEV_KIND_DISK && buf[510] == 0x55 &&
                   buf[511] == 0xAA) {
            dev->fs = BLOCKDEV_FS_PARTITIONED;
        }
    }
    free_mem(buf);
    return dev->fs;
}

void blockdev_format_size(uint64_t bytes, char* out, size_t out_size) {
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    uint32_t unit = 0;
    uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && unit < 5) {
        frac = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        unit++;
    }
    if (unit == 0) snprintf(out, out_size, "%llu %s", whole, units[unit]);
    else snprintf(out, out_size, "%llu.%llu %s", whole, frac, units[unit]);
}

/* ---------------------------------------------------------------- probing */

static bool ahci_bd_read(void* ctx, uint64_t lba, uint32_t count, void* buf) {
    return ahci_read((ahci_drive_t*)ctx, lba, count, buf);
}

static bool ahci_bd_write(void* ctx, uint64_t lba, uint32_t count, void* buf) {
    return ahci_write((ahci_drive_t*)ctx, lba, count, buf);
}

static uint32_t probe_ahci(void) {
    extern pci_device_t pci_devices[];
    extern int pci_device_count;
    uint32_t found = 0;

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t* pci = &pci_devices[i];
        if (pci->class_code != AHCI_CLASS_CODE || pci->subclass != AHCI_SUBCLASS ||
            pci->prog_if != AHCI_PROG_IF) continue;

        ahci_controller_t* ctrl = ahci_init(pci);
        if (!ctrl) continue;
        uint8_t drives = ahci_probe_ports(ctrl);
        for (uint8_t d = 0; d < drives; d++) {
            ahci_drive_t* drive = ahci_get_drive(ctrl, d);
            if (!drive || !drive->present || drive->sectors == 0) continue;
            if (blockdev_register_disk("sata", drive->model, drive->sector_size,
                                       drive->sectors, drive, ahci_bd_read,
                                       ahci_bd_write, 4096)) {
                found++;
            }
        }
    }
    return found;
}

// Provided by the NVMe driver when it is built in.
__attribute__((weak)) uint32_t nvme_probe_all(void) { return 0; }

static void scan_all_partitions(void) {
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = &g_devices[i];
        if (!d->used || d->kind != BLOCKDEV_KIND_DISK) continue;
        partition_scan(d);
    }
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        if (g_devices[i].used) blockdev_identify(&g_devices[i]);
    }
}

uint32_t blockdev_probe_all(void) {
    if (!g_probed) {
        g_probed = true;
        uint32_t sata = probe_ahci();
        uint32_t nvme = nvme_probe_all();
        serial_write_str("blockdev: disks found: sata=");
        serial_write_dec(sata);
        serial_write_str(" nvme=");
        serial_write_dec(nvme);
        serial_write_str("\n");
    }
    scan_all_partitions();
    uint32_t disks = 0;
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        if (g_devices[i].used && g_devices[i].kind == BLOCKDEV_KIND_DISK) disks++;
    }
    return disks;
}
