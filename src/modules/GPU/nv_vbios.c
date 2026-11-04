#include "x86_64/nv_hw.h"
#include <stddef.h>

/*
 * NVIDIA VBIOS parsing, as far as GSP boot needs it: the PCI ROM images,
 * the BIT table, the PMU (falcon ucode) table, FWSEC's descriptor and its
 * DMEM mapper (where the FRTS command goes). Layouts from nouveau.
 */

typedef struct {
    const nv_fw_file_t* f;
    nv_vbios_info_t* info;
} rom_t;

static bool rd(const rom_t* r, uint32_t at, void* buf, uint32_t len) {
    if ((uint64_t)at + len > r->f->size) return false;
    return r->f->read(r->f->ctx, at, buf, len);
}

static uint32_t rd8(const rom_t* r, uint32_t at) {
    uint8_t v = 0;
    return rd(r, at, &v, 1) ? v : 0;
}

static uint32_t rd16(const rom_t* r, uint32_t at) {
    uint8_t v[2] = {0, 0};
    rd(r, at, v, 2);
    return (uint32_t)v[0] | (uint32_t)v[1] << 8;
}

static uint32_t rd32(const rom_t* r, uint32_t at) {
    uint8_t v[4] = {0, 0, 0, 0};
    rd(r, at, v, 4);
    return (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
}

// nvbios_addr: table pointers past the first image point into the
// extended (0xE0) image.
static uint32_t map(const rom_t* r, uint32_t addr) {
    if (addr >= r->info->image0_size && r->info->extended_base)
        return addr - r->info->image0_size + r->info->extended_base;
    return addr;
}

const char* nv_vbios_image_name(uint8_t type) {
    switch (type) {
    case NV_VBIOS_IMAGE_X86: return "x86 (legacy)";
    case NV_VBIOS_IMAGE_EFI: return "EFI";
    case 0x70: return "NVIDIA";
    case NV_VBIOS_IMAGE_EXTENDED: return "extended (falcon ucode)";
    default: return "other";
    }
}

// One image of the chain at `base` (nouveau nvbios_imagen).
static bool read_image(const rom_t* r, uint32_t base, nv_vbios_image_t* img, bool* last) {
    uint32_t sig = rd16(r, base);
    if (sig != 0xAA55u && sig != 0xBB77u && sig != 0x4E56u) return false;
    uint32_t pcir = base + rd16(r, base + 0x18);
    uint32_t psig = rd32(r, pcir);
    if (psig != 0x52494350u /* PCIR */ && psig != 0x5344504Eu /* NPDS */) return false;
    img->base = base;
    img->size = rd16(r, pcir + 0x10) * 512u;
    img->type = (uint8_t)rd8(r, pcir + 0x14);
    *last = (rd8(r, pcir + 0x15) & 0x80u) != 0;
    if (img->type == 0x70) {
        *last = true;
    } else {
        // An NVIDIA extension after the PCIR structure overrides size and "last".
        uint32_t npde = (pcir + rd16(r, pcir + 0x0A) + 0x0Fu) & ~0x0Fu;
        if (rd32(r, npde) == 0x4544504Eu /* NPDE */) {
            img->size = rd16(r, npde + 0x08) * 512u;
            *last = (rd8(r, npde + 0x0A) & 0x80u) != 0;
        }
    }
    return img->size != 0;
}

static uint32_t find_bit(const rom_t* r, uint32_t limit) {
    static const uint8_t sig[5] = {0xFF, 0xB8, 'B', 'I', 'T'};
    uint8_t buf[256 + 4];
    for (uint32_t at = 0; at + 5 <= limit; at += 256) {
        uint32_t n = limit - at < sizeof(buf) ? limit - at : (uint32_t)sizeof(buf);
        if (!rd(r, at, buf, n)) return 0;
        for (uint32_t i = 0; i + 5 <= n && i < 256; i++) {
            uint32_t k = 0;
            while (k < 5 && buf[i + k] == sig[k]) k++;
            if (k == 5) return at + i;
        }
    }
    return 0;
}

// A BIT token: its data offset and length; false if absent.
static bool bit_entry(const rom_t* r, uint8_t id, uint32_t* version, uint32_t* offset,
                      uint32_t* length) {
    uint32_t bit = r->info->bit_offset;
    if (!bit) return false;
    uint32_t header = rd8(r, bit + 8), size = rd8(r, bit + 9), count = rd8(r, bit + 10);
    if (header < 12) header = 12;
    if (size < 6) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = bit + header + i * size;
        if (rd8(r, e) != id) continue;
        *version = rd8(r, e + 1);
        *length = rd16(r, e + 2);
        *offset = rd16(r, e + 4);
        return true;
    }
    return false;
}

bool nv_vbios_parse(const nv_fw_file_t* f, nv_vbios_info_t* out, const char** why) {
    for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;
    rom_t r = { f, out };

    // The image chain.
    uint32_t base = 0;
    bool last = false;
    while (!last && out->image_count < NV_VBIOS_MAX_IMAGES) {
        nv_vbios_image_t img;
        if (!read_image(&r, base, &img, &last)) break;
        out->images[out->image_count++] = img;
        if (out->image_count == 1) out->image0_size = img.size;
        else if (img.type == NV_VBIOS_IMAGE_EXTENDED && !out->extended_base) out->extended_base = img.base;
        base += img.size;
    }
    if (!out->image_count) { *why = "no ROM signature (0xAA55)"; return false; }

    out->bit_offset = find_bit(&r, out->image0_size);
    if (!out->bit_offset) { *why = "no BIT table"; return false; }

    uint32_t version = 0, offset = 0, length = 0;
    if (!bit_entry(&r, 'p', &version, &offset, &length)) { *why = "no BIT 'p' (falcon ucode) entry"; return false; }
    if (version != 2 || length < 4) { *why = "unknown BIT 'p' version"; return false; }
    uint32_t table = rd32(&r, map(&r, offset));
    if (!table) { *why = "empty PMU table pointer"; return false; }
    out->pmu_table = map(&r, table);
    out->pmu_version = (uint8_t)rd8(&r, out->pmu_table);
    uint32_t hdr = rd8(&r, out->pmu_table + 1), entry = rd8(&r, out->pmu_table + 2);
    out->pmu_entries = (uint8_t)rd8(&r, out->pmu_table + 3);
    if (entry < 6) { *why = "PMU table entries too small"; return false; }

    for (uint32_t i = 0; i < out->pmu_entries; i++) {
        uint32_t e = out->pmu_table + hdr + i * entry;
        if (rd8(&r, e) != NV_VBIOS_UCODE_FWSEC_PROD) continue;
        out->fwsec_desc = map(&r, rd32(&r, e + 2));
        break;
    }
    if (!out->fwsec_desc) { *why = "no FWSEC ucode (type 0x85) in the PMU table"; return false; }

    uint32_t d = out->fwsec_desc;
    uint32_t h = rd32(&r, d);
    out->desc_version = (h >> 8) & 0xFFu;
    out->desc_size = h >> 16;
    out->stored_size = rd32(&r, d + 4);
    if (out->desc_version == 2) {
        // Turing (nouveau nvfw_falcon_ucode_desc_v2): no signatures in the
        // descriptor; it is loaded through the ACR boot loader (acr/bl.bin)
        // and DMEM sits at its own offset in the image.
        out->imem_load = rd32(&r, d + 24);
        out->interface_offset = rd32(&r, d + 16);
        out->imem_phys = rd32(&r, d + 20);
        out->imem_virt = rd32(&r, d + 28);
        out->imem_sec_base = rd32(&r, d + 32);
        out->imem_sec_size = rd32(&r, d + 36);
        out->dmem_offset = rd32(&r, d + 40);
        out->dmem_phys = rd32(&r, d + 44);
        out->dmem_load = rd32(&r, d + 48);
        if (out->desc_size < 60) { *why = "FWSEC v2 descriptor too short"; return false; }
    } else if (out->desc_version == 3) {
        // Ampere and newer (nvfw_falcon_ucode_desc_v3).
        out->interface_offset = rd32(&r, d + 12);
        out->imem_phys = rd32(&r, d + 16);
        out->imem_load = rd32(&r, d + 20);
        out->imem_virt = rd32(&r, d + 24);
        out->dmem_phys = rd32(&r, d + 28);
        out->dmem_load = rd32(&r, d + 32);
        out->engine_mask = (uint16_t)rd16(&r, d + 36);
        out->ucode_id = (uint8_t)rd8(&r, d + 38);
        out->signature_count = (uint8_t)rd8(&r, d + 39);
        out->dmem_offset = out->imem_load;
        // The descriptor is 44 bytes, then one 3072-bit RSA signature per key.
        if (out->desc_size != 44u + out->signature_count * 384u) {
            *why = "FWSEC descriptor size does not match its signatures";
            return false;
        }
    } else {
        *why = "unknown FWSEC descriptor version (expected 2 or 3)";
        return false;
    }
    out->image_offset = d + out->desc_size;
    if ((uint64_t)out->image_offset + out->stored_size > f->size ||
        (uint64_t)out->imem_load > out->stored_size ||
        (uint64_t)out->dmem_offset + out->dmem_load > out->stored_size) {
        *why = "FWSEC image runs past the ROM";
        return false;
    }

    // The interface table in DMEM lists the mapper.
    uint32_t dmem = out->image_offset + out->dmem_offset;
    uint32_t appif = dmem + out->interface_offset;
    uint32_t ahdr = rd8(&r, appif + 1), asize = rd8(&r, appif + 2), acount = rd8(&r, appif + 3);
    if (out->interface_offset >= out->dmem_load || asize < 8) {
        *why = "no application interface table in FWSEC's DMEM";
        return false;
    }
    for (uint32_t i = 0; i < acount; i++) {
        uint32_t e = appif + ahdr + i * asize;
        if (rd32(&r, e) != NV_FALCON_APPIF_DMEMMAPPER) continue;
        uint32_t at = rd32(&r, e + 4);
        if (at + 64u > out->dmem_load) break;
        if (rd32(&r, dmem + at) != NV_FWSEC_DMAP_SIGNATURE) { *why = "DMEM mapper without its DMAP signature"; return false; }
        out->mapper_offset = dmem + at;
        out->mapper_cmd_in = rd32(&r, dmem + at + 8);
        out->mapper_init_cmd = rd32(&r, dmem + at + 44);
        out->mapper_cmd_mask0 = rd32(&r, dmem + at + 52);
        break;
    }
    if (!out->mapper_offset) { *why = "FWSEC has no DMEM mapper"; return false; }
    *why = "";
    return true;
}
