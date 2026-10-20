#include "x86_64/partition.h"
#include "x86_64/allocator.h"
#include "x86_64/random.h"
#include "serial.h"
#include "string.h"

/* ================================================================
 * On-disk layouts
 * ================================================================ */

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} mbr_entry_t;

typedef struct __attribute__((packed)) {
    char signature[8];          // "EFI PART"
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;          // inclusive
    uint64_t attributes;
    uint16_t name[36];          // UTF-16LE
} gpt_entry_t;

#define GPT_ENTRIES      128u
#define GPT_ENTRY_BYTES  (GPT_ENTRIES * sizeof(gpt_entry_t))   // 16 KiB

/* Type GUIDs in on-disk (mixed endian) byte order. */
static const uint8_t GUID_ESP[16] = {   // C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const uint8_t GUID_BIOS_BOOT[16] = { // 21686148-6449-6E6F-744E-656564454649
    0x48,0x61,0x68,0x21,0x49,0x64,0x6F,0x6E,0x74,0x4E,0x65,0x65,0x64,0x45,0x46,0x49 };
static const uint8_t GUID_BASIC_DATA[16] = { // EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 };
static const uint8_t GUID_MINIMAFS[16] = {   // 4D696E69-6D61-4653-B0A7-4D4F534D4653
    0x69,0x6E,0x69,0x4D,0x61,0x6D,0x53,0x46,0xB0,0xA7,0x4D,0x4F,0x53,0x4D,0x46,0x53 };

/* ================================================================
 * Helpers
 * ================================================================ */

static uint32_t g_crc_table[256];
static bool g_crc_ready;

uint32_t partition_crc32(const void* data, uint32_t len) {
    if (!g_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc_table[i] = c;
        }
        g_crc_ready = true;
    }
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) crc = g_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void partition_guid_to_string(const uint8_t g[16], char out[37]) {
    static const char hex[] = "0123456789abcdef";
    // First three fields are little endian on disk.
    const int order[16] = { 3,2,1,0, 5,4, 7,6, 8,9, 10,11,12,13,14,15 };
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        uint8_t b = g[order[i]];
        out[o++] = hex[b >> 4];
        out[o++] = hex[b & 15];
    }
    out[o] = '\0';
}

static bool guid_is_zero(const uint8_t g[16]) {
    for (int i = 0; i < 16; i++) if (g[i]) return false;
    return true;
}

static void guid_random(uint8_t g[16]) {
    random_bytes(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   // version 4
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);   // variant
}

static uint64_t align_lba(blockdev_t* disk) {
    uint64_t a = PART_ALIGN_BYTES / disk->sector_size;
    return a ? a : 1;
}

static uint64_t round_up(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

static uint8_t* read_sectors(blockdev_t* disk, uint64_t lba, uint32_t count) {
    uint8_t* buf = (uint8_t*)alloc((uint64_t)count * disk->sector_size);
    if (!buf) return NULL;
    if (!blockdev_read(disk, lba, count, buf)) {
        free_mem(buf);
        return NULL;
    }
    return buf;
}

/* ================================================================
 * GPT in memory
 * ================================================================ */

typedef struct {
    gpt_header_t header;
    gpt_entry_t* entries;       // GPT_ENTRIES
    uint32_t entry_sectors;
} gpt_t;

static uint32_t gpt_entry_sectors(blockdev_t* disk) {
    return (uint32_t)((GPT_ENTRY_BYTES + disk->sector_size - 1) / disk->sector_size);
}

static bool gpt_load(blockdev_t* disk, gpt_t* g) {
    memset(g, 0, sizeof(*g));
    uint8_t* hdr = read_sectors(disk, 1, 1);
    if (!hdr) return false;
    memcpy(&g->header, hdr, sizeof(gpt_header_t));
    free_mem(hdr);

    gpt_header_t* h = &g->header;
    if (memcmp(h->signature, "EFI PART", 8) != 0 || h->header_size < 92 ||
        h->header_size > disk->sector_size) return false;
    uint32_t saved = h->header_crc32;
    h->header_crc32 = 0;
    uint32_t crc = partition_crc32(h, 92);
    h->header_crc32 = saved;
    if (crc != saved) {
        serial_write_str("partition: GPT header CRC mismatch\n");
        return false;
    }
    if (h->entry_size != sizeof(gpt_entry_t) || h->num_entries == 0 ||
        h->num_entries > GPT_ENTRIES) {
        serial_write_str("partition: unsupported GPT entry layout\n");
        return false;
    }

    g->entry_sectors = gpt_entry_sectors(disk);
    uint8_t* raw = read_sectors(disk, h->entries_lba, g->entry_sectors);
    if (!raw) return false;
    g->entries = (gpt_entry_t*)alloc(GPT_ENTRY_BYTES);
    if (!g->entries) { free_mem(raw); return false; }
    memcpy(g->entries, raw, (uint64_t)h->num_entries * sizeof(gpt_entry_t));
    free_mem(raw);
    if (partition_crc32(g->entries, h->num_entries * sizeof(gpt_entry_t)) !=
        h->entries_crc32) {
        serial_write_str("partition: GPT entry array CRC mismatch\n");
        free_mem(g->entries);
        g->entries = NULL;
        return false;
    }
    return true;
}

static void gpt_free(gpt_t* g) {
    if (g->entries) free_mem(g->entries);
    g->entries = NULL;
}

static bool write_protective_mbr(blockdev_t* disk) {
    uint8_t* mbr = (uint8_t*)alloc(disk->sector_size);
    if (!mbr) return false;
    // Keep any boot code already in the first 440 bytes (GRUB's boot.img).
    blockdev_read(disk, 0, 1, mbr);
    memset(mbr + 446, 0, 64);
    mbr_entry_t* e = (mbr_entry_t*)(mbr + 446);
    e->type = PART_MBR_TYPE_GPT;
    e->chs_first[1] = 0x02;
    e->chs_last[0] = e->chs_last[1] = e->chs_last[2] = 0xFF;
    e->lba_first = 1;
    uint64_t n = disk->sectors - 1;
    e->sectors = n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    bool ok = blockdev_write(disk, 0, 1, mbr);
    free_mem(mbr);
    return ok;
}

// Writes primary and backup copies of header and entries.
static bool gpt_store(blockdev_t* disk, gpt_t* g) {
    gpt_header_t* h = &g->header;
    uint64_t last = disk->sectors - 1;
    h->num_entries = GPT_ENTRIES;
    h->entry_size = sizeof(gpt_entry_t);
    h->entries_crc32 = partition_crc32(g->entries, GPT_ENTRY_BYTES);

    uint32_t es = g->entry_sectors;
    uint8_t* sec = (uint8_t*)alloc((uint64_t)es * disk->sector_size);
    uint8_t* hsec = (uint8_t*)alloc(disk->sector_size);
    if (!sec || !hsec) {
        if (sec) free_mem(sec);
        if (hsec) free_mem(hsec);
        return false;
    }
    memcpy(sec, g->entries, GPT_ENTRY_BYTES);

    bool ok = true;
    // Backup first, so a crash mid-way leaves the primary intact.
    gpt_header_t backup = *h;
    backup.my_lba = last;
    backup.alternate_lba = 1;
    backup.entries_lba = last - es;
    backup.header_crc32 = 0;
    backup.header_crc32 = partition_crc32(&backup, 92);
    memset(hsec, 0, disk->sector_size);
    memcpy(hsec, &backup, sizeof(backup));
    ok = ok && blockdev_write(disk, last - es, es, sec);
    ok = ok && blockdev_write(disk, last, 1, hsec);

    h->my_lba = 1;
    h->alternate_lba = last;
    h->entries_lba = 2;
    h->header_crc32 = 0;
    h->header_crc32 = partition_crc32(h, 92);
    memset(hsec, 0, disk->sector_size);
    memcpy(hsec, h, sizeof(*h));
    ok = ok && blockdev_write(disk, 2, es, sec);
    ok = ok && blockdev_write(disk, 1, 1, hsec);
    ok = ok && write_protective_mbr(disk);

    free_mem(sec);
    free_mem(hsec);
    return ok;
}

/* ================================================================
 * Table detection and scanning
 * ================================================================ */

partition_table_t partition_table_type(blockdev_t* disk) {
    if (!disk || disk->kind != BLOCKDEV_KIND_DISK) return PART_TABLE_NONE;
    uint8_t* mbr = read_sectors(disk, 0, 1);
    if (!mbr) return PART_TABLE_NONE;
    partition_table_t type = PART_TABLE_NONE;
    if (memcmp(mbr, "@MAGIC:", 7) != 0 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
        mbr_entry_t* e = (mbr_entry_t*)(mbr + 446);
        bool protective = false, sane = true;
        for (int i = 0; i < 4; i++) {
            if (e[i].type == 0) continue;
            if (e[i].type == PART_MBR_TYPE_GPT) protective = true;
            if (e[i].status != 0 && e[i].status != 0x80) sane = false;
        }
        // A FAT boot sector also ends in 55 AA but is not a table.
        bool fat_vbr = memcmp(mbr + 0x36, "FAT", 3) == 0 ||
                       memcmp(mbr + 0x52, "FAT32", 5) == 0;
        if (protective) type = PART_TABLE_GPT;
        else if (sane && !fat_vbr) type = PART_TABLE_MBR;
    }
    free_mem(mbr);
    return type;
}

const char* partition_table_name(partition_table_t type) {
    switch (type) {
        case PART_TABLE_MBR: return "MBR";
        case PART_TABLE_GPT: return "GPT";
        default: return "none";
    }
}

static void utf16_to_ascii(const uint16_t* in, uint32_t max, char* out, uint32_t out_size) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < max && o + 1 < out_size && in[i]; i++) {
        out[o++] = in[i] < 128 ? (char)in[i] : '?';
    }
    out[o] = '\0';
}

uint32_t partition_scan(blockdev_t* disk) {
    if (!disk || disk->kind != BLOCKDEV_KIND_DISK) return 0;
    blockdev_remove_partitions(disk);

    partition_table_t type = partition_table_type(disk);
    uint32_t found = 0;

    if (type == PART_TABLE_GPT) {
        gpt_t g;
        if (!gpt_load(disk, &g)) return 0;
        for (uint32_t i = 0; i < g.header.num_entries; i++) {
            gpt_entry_t* e = &g.entries[i];
            if (guid_is_zero(e->type_guid) || e->last_lba < e->first_lba) continue;
            blockdev_t* p = blockdev_add_partition(disk, i + 1, e->first_lba,
                                                   e->last_lba - e->first_lba + 1);
            if (!p) continue;
            memcpy(p->type_guid, e->type_guid, 16);
            memcpy(p->part_guid, e->unique_guid, 16);
            p->mbr_type = 0;
            utf16_to_ascii(e->name, 36, p->label, sizeof(p->label));
            found++;
        }
        gpt_free(&g);
    } else if (type == PART_TABLE_MBR) {
        uint8_t* mbr = read_sectors(disk, 0, 1);
        if (!mbr) return 0;
        mbr_entry_t* e = (mbr_entry_t*)(mbr + 446);
        for (uint32_t i = 0; i < 4; i++) {
            if (e[i].type == 0 || e[i].sectors == 0) continue;
            blockdev_t* p = blockdev_add_partition(disk, i + 1, e[i].lba_first,
                                                   e[i].sectors);
            if (!p) continue;
            p->mbr_type = e[i].type;
            memset(p->type_guid, 0, 16);
            memset(p->part_guid, 0, 16);
            // MBR has no unique id; synthesize one from the disk signature.
            memcpy(p->part_guid, mbr + 440, 4);
            p->part_guid[15] = (uint8_t)(i + 1);
            p->label[0] = '\0';
            found++;
        }
        free_mem(mbr);
    }
    return found;
}

/* ================================================================
 * Kinds
 * ================================================================ */

partition_kind_t partition_kind_of(const blockdev_t* p) {
    if (!p || p->kind != BLOCKDEV_KIND_PARTITION) return PART_KIND_OTHER;
    if (p->mbr_type) {
        switch (p->mbr_type) {
            case PART_MBR_TYPE_MINIMAFS: return PART_KIND_MINIMAFS;
            case PART_MBR_TYPE_ESP: return PART_KIND_ESP;
            case 0x0B: case 0x0C: return PART_KIND_FAT;
            default: return PART_KIND_OTHER;
        }
    }
    if (memcmp(p->type_guid, GUID_MINIMAFS, 16) == 0) return PART_KIND_MINIMAFS;
    if (memcmp(p->type_guid, GUID_ESP, 16) == 0) return PART_KIND_ESP;
    if (memcmp(p->type_guid, GUID_BIOS_BOOT, 16) == 0) return PART_KIND_BIOS_BOOT;
    if (memcmp(p->type_guid, GUID_BASIC_DATA, 16) == 0) return PART_KIND_FAT;
    return PART_KIND_OTHER;
}

const char* partition_kind_name(partition_kind_t kind) {
    switch (kind) {
        case PART_KIND_MINIMAFS: return "minimafs";
        case PART_KIND_ESP: return "esp";
        case PART_KIND_BIOS_BOOT: return "biosboot";
        case PART_KIND_FAT: return "fat";
        default: return "other";
    }
}

bool partition_kind_from_name(const char* name, partition_kind_t* kind) {
    if (!name || !kind) return false;
    static const struct { const char* n; partition_kind_t k; } map[] = {
        { "minimafs", PART_KIND_MINIMAFS }, { "mfs", PART_KIND_MINIMAFS },
        { "esp", PART_KIND_ESP }, { "efi", PART_KIND_ESP },
        { "biosboot", PART_KIND_BIOS_BOOT }, { "bios", PART_KIND_BIOS_BOOT },
        { "fat", PART_KIND_FAT }, { "fat32", PART_KIND_FAT },
    };
    for (uint32_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].n) == 0) { *kind = map[i].k; return true; }
    }
    return false;
}

/* ================================================================
 * Creating tables and partitions
 * ================================================================ */

bool partition_create_table(blockdev_t* disk, partition_table_t type) {
    if (!disk || disk->kind != BLOCKDEV_KIND_DISK) return false;
    uint64_t min_sectors = (4ULL * 1024 * 1024) / disk->sector_size;
    if (disk->sectors < min_sectors) return false;

    if (type == PART_TABLE_AUTO) type = PART_TABLE_GPT;
    if (type == PART_TABLE_MBR && disk->sectors > 0xFFFFFFFFull) {
        serial_write_str("partition: disk too big for MBR, use GPT\n");
        return false;
    }

    // Wipe the first MiB (old tables, old MinimaFS storage.desc) and the
    // last 64 sectors (old backup GPT).
    uint32_t wipe = (uint32_t)(PART_ALIGN_BYTES / disk->sector_size);
    uint8_t* zero = (uint8_t*)alloc((uint64_t)wipe * disk->sector_size);
    if (!zero) return false;
    bool ok = blockdev_write(disk, 0, wipe, zero);
    uint32_t tail = 64 < disk->sectors ? 64 : (uint32_t)disk->sectors;
    ok = ok && blockdev_write(disk, disk->sectors - tail, tail, zero);
    free_mem(zero);
    if (!ok) return false;

    if (type == PART_TABLE_MBR) {
        uint8_t* mbr = (uint8_t*)alloc(disk->sector_size);
        if (!mbr) return false;
        uint32_t sig = (uint32_t)random_u64();
        memcpy(mbr + 440, &sig, 4);
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        ok = blockdev_write(disk, 0, 1, mbr);
        free_mem(mbr);
    } else {
        gpt_t g;
        memset(&g, 0, sizeof(g));
        g.entry_sectors = gpt_entry_sectors(disk);
        g.entries = (gpt_entry_t*)alloc(GPT_ENTRY_BYTES);
        if (!g.entries) return false;
        gpt_header_t* h = &g.header;
        memcpy(h->signature, "EFI PART", 8);
        h->revision = 0x00010000;
        h->header_size = 92;
        h->first_usable_lba = 2 + g.entry_sectors;
        h->last_usable_lba = disk->sectors - 2 - g.entry_sectors;
        guid_random(h->disk_guid);
        ok = gpt_store(disk, &g);
        gpt_free(&g);
    }
    partition_scan(disk);
    return ok;
}

typedef struct { uint64_t start, end; } range_t;   // end exclusive

// Collects used ranges (sorted) of the current table.
static uint32_t used_ranges(blockdev_t* disk, range_t* out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < BLOCKDEV_MAX && n < max; i++) {
        blockdev_t* p = blockdev_get(i);
        if (!p || p->parent != disk) continue;
        out[n].start = p->start_lba;
        out[n].end = p->start_lba + p->sectors;
        n++;
    }
    for (uint32_t i = 1; i < n; i++) {
        range_t r = out[i];
        uint32_t j = i;
        while (j > 0 && out[j - 1].start > r.start) { out[j] = out[j - 1]; j--; }
        out[j] = r;
    }
    return n;
}

static bool find_space(blockdev_t* disk, uint64_t first, uint64_t last_excl,
                       uint64_t* start, uint64_t* sectors) {
    range_t used[GPT_ENTRIES];
    uint32_t n = used_ranges(disk, used, GPT_ENTRIES);
    uint64_t a = align_lba(disk);
    uint64_t want_start = *start, want = *sectors;

    uint64_t cursor = first;
    for (uint32_t i = 0; i <= n; i++) {
        uint64_t gap_end = (i < n) ? used[i].start : last_excl;
        if (gap_end > last_excl) gap_end = last_excl;
        uint64_t gap_start = round_up(cursor, a);
        if (want_start) {
            if (want_start >= cursor && want_start < gap_end) {
                uint64_t avail = gap_end - want_start;
                uint64_t len = want ? want : avail;
                if (len <= avail) { *start = want_start; *sectors = len; return true; }
                return false;
            }
        } else if (gap_start < gap_end) {
            uint64_t avail = gap_end - gap_start;
            uint64_t len = want ? want : avail;
            // Whole-MiB sizes keep the next partition aligned too.
            if (!want && len > a) len = len / a * a;
            if (len <= avail && len > 0) { *start = gap_start; *sectors = len; return true; }
        }
        if (i < n && used[i].end > cursor) cursor = used[i].end;
    }
    return false;
}

static void ascii_to_utf16(const char* in, uint16_t* out, uint32_t max) {
    uint32_t i = 0;
    for (; in && in[i] && i < max; i++) out[i] = (uint8_t)in[i];
    for (; i < max; i++) out[i] = 0;
}

blockdev_t* partition_add(blockdev_t* disk, uint64_t start_lba,
                          uint64_t sectors, partition_kind_t kind,
                          const char* label) {
    partition_table_t type = partition_table_type(disk);
    if (type == PART_TABLE_NONE) {
        serial_write_str("partition: disk has no partition table\n");
        return NULL;
    }
    partition_scan(disk);

    uint32_t number = 0;
    if (type == PART_TABLE_GPT) {
        gpt_t g;
        if (!gpt_load(disk, &g)) return NULL;
        uint64_t s = start_lba, n = sectors;
        uint32_t slot = GPT_ENTRIES;
        for (uint32_t i = 0; i < GPT_ENTRIES; i++) {
            if (guid_is_zero(g.entries[i].type_guid)) { slot = i; break; }
        }
        if (slot == GPT_ENTRIES ||
            !find_space(disk, g.header.first_usable_lba,
                        g.header.last_usable_lba + 1, &s, &n)) {
            gpt_free(&g);
            return NULL;
        }
        gpt_entry_t* e = &g.entries[slot];
        memset(e, 0, sizeof(*e));
        const uint8_t* t = GUID_MINIMAFS;
        if (kind == PART_KIND_ESP) t = GUID_ESP;
        else if (kind == PART_KIND_BIOS_BOOT) t = GUID_BIOS_BOOT;
        else if (kind == PART_KIND_FAT) t = GUID_BASIC_DATA;
        memcpy(e->type_guid, t, 16);
        guid_random(e->unique_guid);
        e->first_lba = s;
        e->last_lba = s + n - 1;
        ascii_to_utf16(label ? label : partition_kind_name(kind), e->name, 36);
        bool ok = gpt_store(disk, &g);
        gpt_free(&g);
        if (!ok) return NULL;
        number = slot + 1;
    } else {
        if (kind == PART_KIND_BIOS_BOOT) return NULL;   // GPT only
        uint8_t* mbr = read_sectors(disk, 0, 1);
        if (!mbr) return NULL;
        mbr_entry_t* e = (mbr_entry_t*)(mbr + 446);
        uint32_t slot = 4;
        for (uint32_t i = 0; i < 4; i++) if (e[i].type == 0) { slot = i; break; }
        uint64_t s = start_lba, n = sectors;
        uint64_t limit = disk->sectors < 0xFFFFFFFFull ? disk->sectors : 0xFFFFFFFFull;
        if (slot == 4 || !find_space(disk, 1, limit, &s, &n)) {
            free_mem(mbr);
            return NULL;
        }
        memset(&e[slot], 0, sizeof(mbr_entry_t));
        e[slot].type = kind == PART_KIND_ESP ? PART_MBR_TYPE_ESP :
                       kind == PART_KIND_FAT ? PART_MBR_TYPE_FAT32LBA :
                       PART_MBR_TYPE_MINIMAFS;
        // CHS fields: "use LBA" values.
        e[slot].chs_first[0] = 0xFE; e[slot].chs_first[1] = 0xFF; e[slot].chs_first[2] = 0xFF;
        e[slot].chs_last[0] = 0xFE; e[slot].chs_last[1] = 0xFF; e[slot].chs_last[2] = 0xFF;
        e[slot].lba_first = (uint32_t)s;
        e[slot].sectors = (uint32_t)n;
        // The first partition is the active one (old BIOSes want one).
        bool active = false;
        for (uint32_t i = 0; i < 4; i++) if (e[i].status == 0x80) active = true;
        if (!active) e[slot].status = 0x80;
        bool ok = blockdev_write(disk, 0, 1, mbr);
        free_mem(mbr);
        if (!ok) return NULL;
        number = slot + 1;
    }

    partition_scan(disk);
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* p = blockdev_get(i);
        if (p && p->parent == disk && p->part_number == number) {
            // Make sure no stale filesystem is detected in the new space.
            uint32_t z = 4096 / p->sector_size;
            uint8_t* zero = (uint8_t*)alloc(4096);
            if (zero) {
                blockdev_write(p, 0, z ? z : 1, zero);
                free_mem(zero);
            }
            blockdev_identify(p);
            return p;
        }
    }
    return NULL;
}

bool partition_delete(blockdev_t* disk, uint32_t number) {
    partition_table_t type = partition_table_type(disk);
    if (number == 0) return false;
    for (uint32_t i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* p = blockdev_get(i);
        if (p && p->parent == disk && p->part_number == number &&
            p->mounted_drive >= 0) {
            serial_write_str("partition: refusing to delete a mounted partition\n");
            return false;
        }
    }
    bool ok = false;
    if (type == PART_TABLE_GPT) {
        gpt_t g;
        if (!gpt_load(disk, &g)) return false;
        if (number <= GPT_ENTRIES && !guid_is_zero(g.entries[number - 1].type_guid)) {
            memset(&g.entries[number - 1], 0, sizeof(gpt_entry_t));
            ok = gpt_store(disk, &g);
        }
        gpt_free(&g);
    } else if (type == PART_TABLE_MBR && number <= 4) {
        uint8_t* mbr = read_sectors(disk, 0, 1);
        if (!mbr) return false;
        mbr_entry_t* e = (mbr_entry_t*)(mbr + 446);
        if (e[number - 1].type != 0) {
            memset(&e[number - 1], 0, sizeof(mbr_entry_t));
            ok = blockdev_write(disk, 0, 1, mbr);
        }
        free_mem(mbr);
    }
    partition_scan(disk);
    return ok;
}
