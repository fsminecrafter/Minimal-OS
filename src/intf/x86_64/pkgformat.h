#ifndef PKGFORMAT_H
#define PKGFORMAT_H

#include <stdint.h>

/*
 * MinimalOS Package (.mpkg) archive format.
 *
 * A .mpkg file is a single MinimaFS file used to distribute a set of
 * files and directories as an installable package - the MinimalOS
 * analog of a .zip. Unlike .run bundles (runfile.h), which only ever
 * hold one main.elf plus resources, a .mpkg archive can hold an
 * arbitrary directory tree and each file may be stored either raw or
 * LZSS-compressed (see x86_64/lzss.h).
 *
 * Layout (all integers little-endian):
 *
 *   header                    (MPKG_HEADER_SIZE bytes)
 *   entry[header.entry_count] (MPKG_ENTRY_SIZE bytes each)
 *   payload bytes for every file entry, referenced by that entry's
 *   data_offset/compressed_size (directory entries have no payload)
 *
 * Header:
 *   char     magic[8]      "MPKG0001"
 *   uint32_t version        MPKG_VERSION
 *   uint32_t entry_count
 *
 * Entry:
 *   char     name[128]           relative path, e.g. "programs/hi.run"
 *   uint32_t flags                MPKG_FLAG_*
 *   uint32_t method                MPKG_METHOD_*
 *   uint32_t uncompressed_size
 *   uint32_t compressed_size       == uncompressed_size for STORE
 *   uint32_t data_offset           absolute offset into the archive file
 *
 * Built on the host with tools/pkgbuilder/mkpkg.py.
 */

#define MPKG_MAGIC       "MPKG0001"
#define MPKG_MAGIC_SIZE  8
#define MPKG_VERSION     1

#define MPKG_HEADER_SIZE 16
#define MPKG_NAME_SIZE   128
#define MPKG_ENTRY_SIZE  148   // name[128] + 5 * uint32_t

#define MPKG_FLAG_DIRECTORY (1u << 0)

#define MPKG_METHOD_STORE 0u   // payload stored as-is
#define MPKG_METHOD_LZSS  1u   // payload is LZSS-compressed

#endif // PKGFORMAT_H
