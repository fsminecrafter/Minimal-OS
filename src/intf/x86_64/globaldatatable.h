#ifndef GLOBAL_DATA_TABLE_H
#define GLOBAL_DATA_TABLE_H

#include <stdbool.h>
#include <stdint.h>

bool addvar(uint32_t variable, char name);
uint32_t findvar(char name);

#endif // GLOBAL_DATA_TABLE_H