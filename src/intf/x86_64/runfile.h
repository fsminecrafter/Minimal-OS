#ifndef RUNFILE_H
#define RUNFILE_H

#include <stdint.h>

/* A .run file is a normal MinimaFS file whose data is this archive. */
#define MINIMALOS_RUN_MAGIC       "MINIRUN1"
#define MINIMALOS_RUN_MAGIC_SIZE  8
#define MINIMALOS_RUN_VERSION     1
#define MINIMALOS_RUN_HEADER_SIZE 16
#define MINIMALOS_RUN_ENTRY_SIZE  72
#define MINIMALOS_RUN_NAME_SIZE   64

/* Header (little endian): magic[8], version, entry_count. */
/* Each entry is name[64], payload_offset, payload_size. */

#endif