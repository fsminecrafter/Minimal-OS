#include "x86_64/loader/elfloader.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "string.h"
#include "serial.h"

#define ELF_MAX_FILE_SIZE   (16u * 1024 * 1024)
#define ELF_MAX_PHNUM       64
#define ELF_MAX_IMAGE_SIZE  (64u * 1024 * 1024)

bool elf_load_buffer(uint8_t* filebuf, uint32_t file_size,
                     elf_loaded_image_t* out) {
    if (!filebuf || !out) return false;
    memset(out, 0, sizeof(*out));

    if (file_size < sizeof(Elf64_Ehdr) || file_size > ELF_MAX_FILE_SIZE) {
        serial_write_str("ELF: bad file size\n");
        free_mem(filebuf);
        return false;
    }

    Elf64_Ehdr* eh = (Elf64_Ehdr*)filebuf;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        serial_write_str("ELF: bad magic/class (need little-endian ELF64)\n");
        free_mem(filebuf);
        return false;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_write_str("ELF: wrong machine (need x86_64)\n");
        free_mem(filebuf);
        return false;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        serial_write_str("ELF: unsupported e_type (need EXEC or DYN)\n");
        free_mem(filebuf);
        return false;
    }
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHNUM) {
        serial_write_str("ELF: bad phnum\n");
        free_mem(filebuf);
        return false;
    }
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > file_size) {
        serial_write_str("ELF: program headers out of range\n");
        free_mem(filebuf);
        return false;
    }

    Elf64_Phdr* phdrs = (Elf64_Phdr*)(filebuf + eh->e_phoff);

    // Compute total image span from PT_LOAD segments. Segments must
    // already be base-0 relative - see sdk/link.ld.
    uint64_t image_end = 0;
    Elf64_Phdr* dynamic_ph = NULL;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr* ph = &phdrs[i];

        if (ph->p_type == PT_DYNAMIC) {
            dynamic_ph = ph;
            continue;
        }
        if (ph->p_type != PT_LOAD) continue;

        if (ph->p_filesz > ph->p_memsz) {
            serial_write_str("ELF: filesz > memsz\n");
            free_mem(filebuf);
            return false;
        }
        if ((uint64_t)ph->p_offset + ph->p_filesz > file_size) {
            serial_write_str("ELF: segment data out of range\n");
            free_mem(filebuf);
            return false;
        }

        uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
        if (seg_end < ph->p_vaddr) { // overflow
            serial_write_str("ELF: segment overflow\n");
            free_mem(filebuf);
            return false;
        }
        if (seg_end > image_end) image_end = seg_end;
    }

    if (image_end == 0 || image_end > ELF_MAX_IMAGE_SIZE) {
        serial_write_str("ELF: image empty or too large\n");
        free_mem(filebuf);
        return false;
    }

    // alloc() zero-fills, which is exactly what .bss needs.
    uint8_t* base = (uint8_t*)alloc(image_end);
    if (!base) {
        serial_write_str("ELF: OOM allocating image\n");
        free_mem(filebuf);
        return false;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;
        memcpy(base + ph->p_vaddr, filebuf + ph->p_offset, ph->p_filesz);
    }

    // Apply R_X86_64_RELATIVE relocations - the only kind a fully
    // self-contained, statically-linked PIE with no external symbols
    // (exactly what the SDK produces) ever needs.
    if (dynamic_ph) {
        Elf64_Dyn* dyn = (Elf64_Dyn*)(base + dynamic_ph->p_vaddr);
        uint64_t dyn_count = dynamic_ph->p_memsz / sizeof(Elf64_Dyn);
        uint64_t rela_off = 0, rela_size = 0, rela_ent = sizeof(Elf64_Rela);

        for (uint64_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
            if (dyn[i].d_tag == DT_RELA)        rela_off  = dyn[i].d_val;
            else if (dyn[i].d_tag == DT_RELASZ) rela_size = dyn[i].d_val;
            else if (dyn[i].d_tag == DT_RELAENT) rela_ent = dyn[i].d_val;
        }

        if (rela_off && rela_size && rela_ent >= sizeof(Elf64_Rela) &&
            rela_off + rela_size <= image_end) {
            uint64_t count = rela_size / rela_ent;
            for (uint64_t i = 0; i < count; i++) {
                Elf64_Rela* r = (Elf64_Rela*)(base + rela_off + i * rela_ent);
                uint32_t type = (uint32_t)(r->r_info & 0xffffffffu);
                if (type == R_X86_64_RELATIVE) {
                    *(uint64_t*)(base + r->r_offset) =
                        (uint64_t)(uintptr_t)base + (uint64_t)r->r_addend;
                }
                // Any other relocation type references an external
                // symbol we have no dynamic linker to resolve. The SDK
                // is expected never to produce these; left unfixed
                // rather than aborting the whole load.
            }
        }
    }

    free_mem(filebuf);

    out->base        = base;
    out->image_size  = image_end;
    out->entry_point = (uint64_t)(uintptr_t)base + eh->e_entry;

    serial_write_str("ELF: loaded buffer base=0x"); serial_write_hex((uint64_t)(uintptr_t)base);
    serial_write_str(" size="); serial_write_dec(image_end);
    serial_write_str(" entry=0x"); serial_write_hex(out->entry_point);
    serial_write_str("\n");

    return true;
}

bool elf_load_file(const char* path, elf_loaded_image_t* out) {
    if (!path || !out) return false;

    minimafs_file_handle_t* f = minimafs_open(path, true);
    if (!f) {
        serial_write_str("ELF: cannot open "); serial_write_str(path); serial_write_str("\n");
        return false;
    }

    uint32_t file_size = minimafs_size(f);
    uint8_t* filebuf = (uint8_t*)alloc_unzeroed(file_size);
    if (!filebuf) {
        serial_write_str("ELF: OOM reading file\n");
        minimafs_close(f);
        return false;
    }

    uint32_t got = minimafs_read(f, filebuf, file_size);
    minimafs_close(f);
    if (got != file_size) {
        serial_write_str("ELF: short read\n");
        free_mem(filebuf);
        return false;
    }
    return elf_load_buffer(filebuf, file_size, out);
}

void elf_unload(elf_loaded_image_t* image) {
    if (!image || !image->base) return;
    free_mem(image->base);
    image->base = NULL;
    image->image_size = 0;
    image->entry_point = 0;
}