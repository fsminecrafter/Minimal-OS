#ifndef MMU_TABLES_H
#define MMU_TABLES_H

#include <stdint.h>

extern uint64_t page_table_l4[512];
extern uint64_t page_table_l3[512];
extern uint64_t page_table_l2[2048];   // 4 page directories: 0-4 GiB identity map
#define IDENTITY_MAP_GIB 4
extern uint64_t page_table_l3_mmio[512];
extern uint64_t page_table_l2_mmio[512];

#endif
