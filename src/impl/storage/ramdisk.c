#include "x86_64/ramdisk.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "string.h"

/*
 * A disk in RAM: a buffer registered as an ordinary block device ("ram0"),
 * so MinimaFS, format, the file commands and the storage syscalls work on
 * it unchanged. The 'ramfs' boot option makes it the system volume (see
 * boot_options.h); everything written to it is gone at the next boot.
 */

#define RAMDISK_SECTOR 512u

typedef struct {
    uint8_t* data;
    uint64_t sectors;
} ramdisk_t;

static ramdisk_t g_ramdisk;

static bool ramdisk_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    ramdisk_t* rd = (ramdisk_t*)ctx;
    if (lba + count > rd->sectors) return false;
    memcpy(buffer, rd->data + lba * RAMDISK_SECTOR, (size_t)count * RAMDISK_SECTOR);
    return true;
}

static bool ramdisk_write(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    ramdisk_t* rd = (ramdisk_t*)ctx;
    if (lba + count > rd->sectors) return false;
    memcpy(rd->data + lba * RAMDISK_SECTOR, buffer, (size_t)count * RAMDISK_SECTOR);
    return true;
}

blockdev_t* ramdisk_create(uint32_t mib) {
    if (g_ramdisk.data) return NULL;                 // one is enough
    if (mib < RAMDISK_MIN_MIB) mib = RAMDISK_MIN_MIB;
    // Take what the heap can give, halving down to the minimum.
    uint8_t* data = NULL;
    for (; mib >= RAMDISK_MIN_MIB; mib /= 2) {
        data = (uint8_t*)alloc((size_t)mib << 20);   // zeroed: reads as an empty disk
        if (data) break;
        serial_write_str("ramdisk: not enough memory, trying half\n");
    }
    if (!data) return NULL;
    g_ramdisk.data = data;
    g_ramdisk.sectors = ((uint64_t)mib << 20) / RAMDISK_SECTOR;

    blockdev_t* dev = blockdev_register_disk("ram", "RAM disk", RAMDISK_SECTOR,
                                             g_ramdisk.sectors, &g_ramdisk,
                                             ramdisk_read, ramdisk_write, 0);
    if (!dev) {
        free_mem(data);
        g_ramdisk.data = NULL;
        return NULL;
    }
    serial_write_str("ramdisk: ");
    serial_write_dec(mib);
    serial_write_str(" MiB as ");
    serial_write_str(dev->name);
    serial_write_str("\n");
    return dev;
}
