#include "panic.h"
#include <stdbool.h>
#include <stdint.h>
#include "string.h"
#include "x86_64/globaldatatable.h"

#define MAX_VARS 1096
#define MAX_NAME_LEN 32

typedef struct {
    uint32_t variable;
    char name[MAX_NAME_LEN];
    bool used;
} gdtvar;

gdtvar globallist[MAX_VARS];

uint32_t findvar(const char* name) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (globallist[i].used && strcmp(globallist[i].name, name) == 0) {
            return globallist[i].variable;
        }
    }

    PANIC("Variable not found");
    return 0;
}

bool addvar(uint32_t variable, const char* name) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (globallist[i].used && strcmp(globallist[i].name, name) == 0) {
            globallist[i].variable = variable; // overwrite
            return true;
        }
    }

    for (int i = 0; i < MAX_VARS; i++) {
        if (!globallist[i].used) {
            globallist[i].used = true;
            strncpy(globallist[i].name, name, MAX_NAME_LEN - 1);
            globallist[i].name[MAX_NAME_LEN - 1] = '\0';
            globallist[i].variable = variable;
            return true;
        }
    }

    return false;
}