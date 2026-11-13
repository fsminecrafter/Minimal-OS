#ifndef ELFLOADER_H
#define ELFLOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    bool     page_backed;   // image storage came from PMM pages instead of heap
    size_t   page_count;    // number of PMM pages owned by the image
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

// Loads into temporary storage while rebasing the entry point and
// R_X86_64_RELATIVE relocations for the requested runtime virtual address.
bool elf_load_buffer_at(uint8_t* file_data, uint32_t file_size,
                        uint64_t load_address, elf_loaded_image_t* out);

// Registers a PMM-backed input buffer so elf_load_buffer() can release it.
bool elf_register_page_buffer(void* buffer, size_t page_count);

// Frees the memory owned by a previously loaded image. Do NOT call
// this while a process is still executing code from the image.
void elf_unload(elf_loaded_image_t* image);

// ---- Loading straight into a program's address space (uvm.h) ----
//
// The file is read piece by piece through a callback, so neither the file
// nor the image is ever held in kernel memory as a whole: each PT_LOAD
// segment's bytes go from a small buffer into the program's pages, and
// .bss pages are not touched at all (they read as zero on first use).

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

#define ELF_STREAM_MAX_FILE   (256ull * 1024 * 1024)
#define ELF_STREAM_MAX_IMAGE  (1024ull * 1024 * 1024)
#define ELF_STREAM_MAX_PHNUM  64

// Reads `size` bytes at `offset` of the ELF file; returns how many it read.
typedef uint32_t (*elf_read_fn)(void* ctx, uint64_t offset, void* buffer, uint32_t size);

typedef struct {
    elf_read_fn read;
    void* ctx;
    uint64_t file_size;
    Elf64_Ehdr header;
    Elf64_Phdr segments[ELF_STREAM_MAX_PHNUM];
    uint64_t image_size;        // bytes from the base to the end of the last segment
} elf_stream_t;

struct process;

// Reads and checks the headers. The image must be linked at address 0
// (a static PIE), as the SDK's link.ld produces.
bool elf_stream_open(elf_stream_t* elf, elf_read_fn read, void* ctx, uint64_t file_size);

// Maps the image at `base` in `proc`'s address space, copies the segments
// in, applies R_X86_64_RELATIVE relocations and gives every segment its
// own protection (PF_R/PF_W/PF_X). Returns the entry point through
// `entry`. On failure the caller destroys the process.
bool elf_stream_load(elf_stream_t* elf, struct process* proc, uint64_t base, uint64_t* entry);

#endif // ELFLOADER_H