#include "serial.h"
#include "x86_64/minimafs.h"
#include "x86_64/partition.h"
#include "minimafshandler.h"
#include "string.h"

#include <stdbool.h>

char kernel_boot_root[64];

static minimafs_disk_device_t* g_boot_device;
static bool g_probed;

static bool root_matches(blockdev_t* dev) {
    if (!kernel_boot_root[0]) return false;
    if (strcmp(dev->name, kernel_boot_root) == 0) return true;
    if (dev->kind == BLOCKDEV_KIND_PARTITION) {
        char guid[37];
        partition_guid_to_string(dev->part_guid, guid);
        // Case-insensitive compare of the GUID text.
        const char* a = guid;
        const char* b = kernel_boot_root;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) return false;
            a++; b++;
        }
        return *a == '\0' && *b == '\0';
    }
    return false;
}

// root= wins; otherwise the first MinimaFS volume in probe order (disks
// come before partitions, so an old whole-disk volume keeps being 0:).
static minimafs_disk_device_t* pick_boot_volume(void) {
    blockdev_t* first_mfs = NULL;
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = blockdev_get(i);
        if (!d) continue;
        if (root_matches(d)) return d;
        if (!first_mfs && d->fs == BLOCKDEV_FS_MINIMAFS) first_mfs = d;
    }
    if (kernel_boot_root[0]) {
        serial_write_str("storage: root= volume not found, using the first MinimaFS volume\n");
    }
    return first_mfs ? first_mfs : blockdev_first_disk();
}

minimafs_disk_device_t* initializeminimafs(int driveindex) {
    (void)driveindex;
    if (!g_probed) {
        g_probed = true;
        minimafs_init();
    }
    uint32_t disks = blockdev_probe_all();
    serial_write_str("storage: ");
    serial_write_dec(disks);
    serial_write_str(" disk(s)\n");
    if (disks == 0) {
        g_boot_device = NULL;
        return NULL;
    }
    // Keep the current boot volume while it is mounted.
    if (!g_boot_device || g_boot_device->mounted_drive < 0 || !g_boot_device->used) {
        g_boot_device = pick_boot_volume();
    }
    if (g_boot_device) {
        serial_write_str("storage: boot volume ");
        serial_write_str(g_boot_device->name);
        serial_write_str("\n");
    }
    return g_boot_device;
}

minimafs_disk_device_t* getminimadrive(void) {
    return g_boot_device;
}

int mountdrive(minimafs_disk_device_t* device, int deviceindex) {
    if (!device) return 0;
    int successcode = minimafs_mount(device, (uint8_t)deviceindex);
    if (successcode != 1) {
        serial_write_str("ERROR: Failed to mount filesystem\n");
        return successcode;
    }
    serial_write_str("Filesystem ");
    serial_write_str(device->name);
    serial_write_str(" mounted as drive ");
    serial_write_dec(deviceindex);
    serial_write_str(":\n");
    return successcode;
}

int storage_free_drive_number(int first) {
    for (int n = first; n < MINIMAFS_MAX_DRIVES; n++) {
        minimafs_drive_t* d = get_drive((uint8_t)n);
        if (d && !d->mounted) return n;
    }
    return -1;
}

uint32_t storage_automount_others(void) {
    uint32_t mounted = 0;
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* d = blockdev_get(i);
        if (!d || d->fs != BLOCKDEV_FS_MINIMAFS || d->mounted_drive >= 0) continue;
        // A disk with partitions is not itself a volume.
        if (d == g_boot_device) continue;
        int n = storage_free_drive_number(1);
        if (n < 0) break;
        if (mountdrive(d, n) == 1) mounted++;
    }
    return mounted;
}
