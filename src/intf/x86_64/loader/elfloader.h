#ifndef ELFLOADER_H
#define ELFLOADER_H

#include <stdint.h>
#include <stdbool.h>

// ===========================================
// Minimal ELF64 structures (SysV ABI)
// ===========================================

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} __attribute__((packed)) Elf64_Dyn;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) Elf64_Rela;

#define ELFCLASS64          2
#define ELFDATA2LSB         1
#define ET_EXEC             2
#define ET_DYN              3
#define EM_X86_64           62
#define PT_LOAD             1
#define PT_DYNAMIC          2
#define DT_NULL             0
#define DT_RELA             7
#define DT_RELASZ           8
#define DT_RELAENT          9
#define R_X86_64_RELATIVE   8

// ===========================================
// Loader API
// ===========================================

typedef struct {
    uint64_t entry_point;   // runtime (relocated) entry address
    void*    base;          // heap allocation the image lives in
    uint64_t image_size;    // bytes reserved for the image (== highest vaddr+memsz)
} elf_loaded_image_t;

// Loads a static/PIE ELF64 executable from a MinimaFS path into a
// fresh heap allocation, applies R_X86_64_RELATIVE relocations if a
// PT_DYNAMIC segment is present, and fills in `out` on success.
// The binary MUST be linked with a base address of 0 (see sdk/link.ld) -
// there is no per-process paging to honor any other link address.
bool elf_load_file(const char* path, elf_loaded_image_t* out);

/* Takes ownership of file_data and frees it on success or failure. */
bool elf_load_buffer(uint8_t* file_data, uint32_t file_size,
                     elf_loaded_image_t* out);

// Frees the memory owned by a previously loaded image. Do NOT call
// this while a process is still executing code from the image.
void elf_unload(elf_loaded_image_t* image);

#endif // ELFLOADER_H