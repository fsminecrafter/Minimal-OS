#include "x86_64/fat32.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "string.h"

#define FAT32_RESERVED_SECTORS 32u
#define FAT32_NUM_FATS 2u
#define FAT32_MIN_CLUSTERS 65525u
#define FAT32_MAX_CLUSTERS (4u * 1024u * 1024u)   // keeps the in-memory FAT <= 16 MiB
#define FAT32_MAX_DIRS 32u
#define FAT32_EOC 0x0FFFFFFFu
#define FAT32_IO_CHUNK (64u * 1024u)
#define FAT32_DATE ((uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1))   // 2026-01-01

typedef struct __attribute__((packed)) {
    char name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat_dirent_t;

typedef struct {
    uint32_t first_cluster;
    uint32_t clusters;
    uint32_t capacity;          // entries
    uint32_t count;
    fat_dirent_t* entries;
} fat_dir_t;

struct fat32_builder {
    blockdev_t* dev;
    uint32_t bps;               // bytes per sector
    uint32_t spc;               // sectors per cluster
    uint32_t cluster_bytes;
    uint32_t total_sectors;
    uint32_t fat_sectors;
    uint32_t data_start;        // sector of cluster 2
    uint32_t cluster_count;     // data clusters
    uint32_t next_cluster;
    uint32_t* fat;
    fat_dir_t dirs[FAT32_MAX_DIRS];
    uint32_t dir_count;
    uint8_t* io;                // FAT32_IO_CHUNK bounce buffer
    bool failed;
};

static uint64_t cluster_lba(fat32_builder_t* fs, uint32_t cluster) {
    return fs->data_start + (uint64_t)(cluster - 2) * fs->spc;
}

static uint32_t alloc_clusters(fat32_builder_t* fs, uint32_t n) {
    if (n == 0) return 0;
    if (fs->next_cluster + n > fs->cluster_count + 2) {
        serial_write_str("fat32: volume full\n");
        fs->failed = true;
        return 0;
    }
    uint32_t first = fs->next_cluster;
    for (uint32_t i = 0; i < n; i++) {
        fs->fat[first + i] = (i + 1 < n) ? first + i + 1 : FAT32_EOC;
    }
    fs->next_cluster += n;
    return first;
}

static bool write_zero(fat32_builder_t* fs, uint64_t lba, uint64_t sectors) {
    memset(fs->io, 0, FAT32_IO_CHUNK);
    uint32_t per = FAT32_IO_CHUNK / fs->bps;
    while (sectors > 0) {
        uint32_t n = sectors < per ? (uint32_t)sectors : per;
        if (!blockdev_write(fs->dev, lba, n, fs->io)) return false;
        lba += n;
        sectors -= n;
    }
    return true;
}

static void make_83(const char* name, char out[11]) {
    memset(out, ' ', 11);
    const char* dot = strrchr(name, '.');
    uint32_t o = 0;
    for (const char* p = name; *p && p != dot && o < 8; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[o++] = c;
    }
    if (dot) {
        o = 8;
        for (const char* p = dot + 1; *p && o < 11; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[o++] = c;
        }
    }
}

static uint32_t new_dir(fat32_builder_t* fs, uint32_t parent_cluster) {
    if (fs->dir_count >= FAT32_MAX_DIRS) return 0;
    fat_dir_t* d = &fs->dirs[fs->dir_count];
    uint32_t clusters = 4096 / fs->cluster_bytes;
    if (clusters == 0) clusters = 1;
    d->first_cluster = alloc_clusters(fs, clusters);
    if (!d->first_cluster) return 0;
    d->clusters = clusters;
    d->capacity = clusters * fs->cluster_bytes / sizeof(fat_dirent_t);
    d->count = 0;
    d->entries = (fat_dirent_t*)alloc((uint64_t)d->capacity * sizeof(fat_dirent_t));
    if (!d->entries) return 0;
    fs->dir_count++;

    if (parent_cluster != 0xFFFFFFFFu) {   // not the root: add . and ..
        fat_dirent_t* dot = &d->entries[d->count++];
        memset(dot->name, ' ', 11);
        dot->name[0] = '.';
        dot->attr = 0x10;
        dot->crt_date = dot->wrt_date = dot->lst_acc_date = FAT32_DATE;
        dot->fst_clus_hi = (uint16_t)(d->first_cluster >> 16);
        dot->fst_clus_lo = (uint16_t)d->first_cluster;
        fat_dirent_t* dotdot = &d->entries[d->count++];
        memset(dotdot->name, ' ', 11);
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
        dotdot->attr = 0x10;
        dotdot->crt_date = dotdot->wrt_date = dotdot->lst_acc_date = FAT32_DATE;
        uint32_t pc = parent_cluster == 2 ? 0 : parent_cluster;  // root is 0 in ".."
        dotdot->fst_clus_hi = (uint16_t)(pc >> 16);
        dotdot->fst_clus_lo = (uint16_t)pc;
    }
    return d->first_cluster;
}

static fat_dir_t* find_dir(fat32_builder_t* fs, uint32_t cluster) {
    for (uint32_t i = 0; i < fs->dir_count; i++) {
        if (fs->dirs[i].first_cluster == cluster) return &fs->dirs[i];
    }
    return NULL;
}

static fat_dirent_t* add_entry(fat32_builder_t* fs, uint32_t dir, const char* name,
                               uint8_t attr) {
    fat_dir_t* d = find_dir(fs, dir);
    if (!d || d->count >= d->capacity) {
        fs->failed = true;
        return NULL;
    }
    fat_dirent_t* e = &d->entries[d->count++];
    memset(e, 0, sizeof(*e));
    make_83(name, e->name);
    e->attr = attr;
    e->crt_date = e->wrt_date = e->lst_acc_date = FAT32_DATE;
    return e;
}

fat32_builder_t* fat32_create(blockdev_t* dev, const char* label, uint32_t volume_id) {
    if (!dev || (dev->sector_size != 512 && dev->sector_size != 4096)) return NULL;
    if (dev->sectors > 0xFFFFFFFFull) return NULL;

    fat32_builder_t* fs = (fat32_builder_t*)alloc(sizeof(fat32_builder_t));
    if (!fs) return NULL;
    fs->dev = dev;
    fs->bps = dev->sector_size;
    fs->total_sectors = (uint32_t)dev->sectors;

    // Smallest cluster that still keeps the count in FAT32 range; for an
    // ESP-sized volume that is one sector.
    uint64_t bytes = (uint64_t)fs->total_sectors * fs->bps;
    fs->spc = 1;
    while ((uint64_t)fs->spc * fs->bps < 32768 &&
           bytes / ((uint64_t)fs->spc * fs->bps) > FAT32_MAX_CLUSTERS) {
        fs->spc *= 2;
    }
    fs->cluster_bytes = fs->spc * fs->bps;

    uint32_t per_fat_sector = fs->bps / 4;
    // Smallest FAT that covers every cluster left after the FATs
    // themselves. Start from an underestimate and grow.
    uint32_t avail = fs->total_sectors - FAT32_RESERVED_SECTORS;
    uint32_t fat_sz = avail / (fs->spc * per_fat_sector + FAT32_NUM_FATS);
    if (fat_sz == 0) fat_sz = 1;
    while ((uint64_t)(avail - FAT32_NUM_FATS * fat_sz) / fs->spc + 2 >
           (uint64_t)fat_sz * per_fat_sector) {
        fat_sz++;
    }
    fs->fat_sectors = fat_sz;
    fs->data_start = FAT32_RESERVED_SECTORS + FAT32_NUM_FATS * fat_sz;
    fs->cluster_count = (fs->total_sectors - fs->data_start) / fs->spc;
    if (fs->cluster_count < FAT32_MIN_CLUSTERS || fs->cluster_count > FAT32_MAX_CLUSTERS) {
        serial_write_str("fat32: volume size not suitable for FAT32\n");
        free_mem(fs);
        return NULL;
    }

    fs->fat = (uint32_t*)alloc((uint64_t)(fs->cluster_count + 2) * 4);
    fs->io = (uint8_t*)alloc(FAT32_IO_CHUNK);
    if (!fs->fat || !fs->io) {
        if (fs->fat) free_mem(fs->fat);
        if (fs->io) free_mem(fs->io);
        free_mem(fs);
        return NULL;
    }
    fs->fat[0] = 0x0FFFFFF8;
    fs->fat[1] = FAT32_EOC;
    fs->next_cluster = 2;

    // Boot sector, FSInfo and their backups (sectors 0, 1, 6, 7).
    uint8_t* bs = (uint8_t*)alloc(fs->bps);
    if (!bs) { fat32_finish(fs); return NULL; }
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "MINIMAOS", 8);
    *(uint16_t*)(bs + 11) = (uint16_t)fs->bps;
    bs[13] = (uint8_t)fs->spc;
    *(uint16_t*)(bs + 14) = FAT32_RESERVED_SECTORS;
    bs[16] = FAT32_NUM_FATS;
    bs[21] = 0xF8;
    *(uint16_t*)(bs + 24) = 63;
    *(uint16_t*)(bs + 26) = 255;
    uint64_t hidden = 0;
    for (blockdev_t* p = dev; p && p->kind == BLOCKDEV_KIND_PARTITION; p = p->parent) {
        hidden += p->start_lba;
    }
    *(uint32_t*)(bs + 28) = (uint32_t)hidden;
    *(uint32_t*)(bs + 32) = fs->total_sectors;
    *(uint32_t*)(bs + 36) = fs->fat_sectors;
    *(uint32_t*)(bs + 44) = 2;        // root cluster
    *(uint16_t*)(bs + 48) = 1;        // FSInfo sector
    *(uint16_t*)(bs + 50) = 6;        // backup boot sector
    bs[64] = 0x80;
    bs[66] = 0x29;
    *(uint32_t*)(bs + 67) = volume_id;
    char lab[11];
    make_83(label ? label : "NO NAME", lab);
    memcpy(bs + 71, lab, 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;

    bool ok = write_zero(fs, 0, FAT32_RESERVED_SECTORS);
    ok = ok && blockdev_write(dev, 0, 1, bs) && blockdev_write(dev, 6, 1, bs);
    free_mem(bs);
    if (!ok) { fs->failed = true; fat32_finish(fs); return NULL; }

    uint32_t root = new_dir(fs, 0xFFFFFFFFu);
    if (root != 2) { fs->failed = true; fat32_finish(fs); return NULL; }
    fat_dirent_t* vol = add_entry(fs, root, "", 0x08);
    if (vol) memcpy(vol->name, lab, 11);
    return fs;
}

uint32_t fat32_root(fat32_builder_t* fs) {
    (void)fs;
    return 2;
}

uint32_t fat32_mkdir(fat32_builder_t* fs, uint32_t parent, const char* name) {
    if (!fs || fs->failed) return 0;
    uint32_t cluster = new_dir(fs, parent);
    if (!cluster) { fs->failed = true; return 0; }
    fat_dirent_t* e = add_entry(fs, parent, name, 0x10);
    if (!e) return 0;
    e->fst_clus_hi = (uint16_t)(cluster >> 16);
    e->fst_clus_lo = (uint16_t)cluster;
    return cluster;
}

bool fat32_add_file(fat32_builder_t* fs, uint32_t dir, const char* name,
                    const void* data, uint32_t size) {
    if (!fs || fs->failed) return false;
    uint32_t clusters = (size + fs->cluster_bytes - 1) / fs->cluster_bytes;
    uint32_t first = alloc_clusters(fs, clusters);
    if (clusters && !first) return false;

    // Contiguous clusters, so the file is one run of sectors.
    const uint8_t* src = (const uint8_t*)data;
    uint64_t lba = first ? cluster_lba(fs, first) : 0;
    uint32_t left = size;
    while (left > 0) {
        uint32_t n = left < FAT32_IO_CHUNK ? left : FAT32_IO_CHUNK;
        uint32_t sectors = (n + fs->bps - 1) / fs->bps;
        memset(fs->io + n, 0, sectors * fs->bps - n);
        memcpy(fs->io, src, n);
        if (!blockdev_write(fs->dev, lba, sectors, fs->io)) {
            fs->failed = true;
            return false;
        }
        lba += sectors;
        src += n;
        left -= n;
    }

    fat_dirent_t* e = add_entry(fs, dir, name, 0x20);
    if (!e) return false;
    e->fst_clus_hi = (uint16_t)(first >> 16);
    e->fst_clus_lo = (uint16_t)first;
    e->file_size = size;
    return true;
}

bool fat32_finish(fat32_builder_t* fs) {
    if (!fs) return false;
    bool ok = !fs->failed;

    // Directories.
    for (uint32_t i = 0; i < fs->dir_count && ok; i++) {
        fat_dir_t* d = &fs->dirs[i];
        uint64_t sectors = (uint64_t)d->clusters * fs->spc;
        ok = blockdev_write(fs->dev, cluster_lba(fs, d->first_cluster),
                            (uint32_t)sectors, d->entries);
    }

    // Both FAT copies.
    if (ok) {
        uint64_t fat_bytes = (uint64_t)fs->fat_sectors * fs->bps;
        uint32_t per = FAT32_IO_CHUNK / fs->bps;
        for (uint32_t copy = 0; copy < FAT32_NUM_FATS && ok; copy++) {
            uint64_t base = FAT32_RESERVED_SECTORS + (uint64_t)copy * fs->fat_sectors;
            for (uint32_t s = 0; s < fs->fat_sectors && ok; s += per) {
                uint32_t n = fs->fat_sectors - s < per ? fs->fat_sectors - s : per;
                uint64_t off = (uint64_t)s * fs->bps;
                uint64_t have = (uint64_t)(fs->cluster_count + 2) * 4;
                memset(fs->io, 0, FAT32_IO_CHUNK);
                if (off < have) {
                    uint64_t len = have - off;
                    if (len > (uint64_t)n * fs->bps) len = (uint64_t)n * fs->bps;
                    memcpy(fs->io, (uint8_t*)fs->fat + off, len);
                }
                ok = blockdev_write(fs->dev, base + s, n, fs->io);
            }
        }
        (void)fat_bytes;
    }

    // FSInfo (+ backup).
    if (ok) {
        memset(fs->io, 0, fs->bps);
        *(uint32_t*)(fs->io + 0) = 0x41615252;
        *(uint32_t*)(fs->io + 484) = 0x61417272;
        *(uint32_t*)(fs->io + 488) = fs->cluster_count + 2 - fs->next_cluster;
        *(uint32_t*)(fs->io + 492) = fs->next_cluster;
        *(uint32_t*)(fs->io + 508) = 0xAA550000;
        ok = blockdev_write(fs->dev, 1, 1, fs->io) && blockdev_write(fs->dev, 7, 1, fs->io);
    }

    for (uint32_t i = 0; i < fs->dir_count; i++) {
        if (fs->dirs[i].entries) free_mem(fs->dirs[i].entries);
    }
    if (fs->fat) free_mem(fs->fat);
    if (fs->io) free_mem(fs->io);
    free_mem(fs);
    if (!ok) serial_write_str("fat32: writing the volume failed\n");
    return ok;
}
