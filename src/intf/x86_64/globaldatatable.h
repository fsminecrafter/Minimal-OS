#ifndef GLOBAL_DATA_TABLE_H
#define GLOBAL_DATA_TABLE_H

#include <stdbool.h>
#include <stdint.h>

bool addvar(uint64_t variable, const char* name);
uint64_t findvar(const char* name);

#endif // GLOBAL_DATA_TABLE_H