#include "panic.h"
#include <stdbool.h>
#include <stdint.h>
#include "string.h"
#include "x86_64/globaldatatable.h"

#define MAX_VARS 1096

typedef struct {
    uint32_t variable;
    char name;
    bool used;
} gdtvar;

gdtvar globallist[MAX_VARS];

uint32_t findvar(char name) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (globallist[i].used && globallist[i].name == name) {
            return globallist[i].variable;
        }
    }

    PANIC("Variable not found");
    return 0;
}

bool addvar(uint32_t variable, char name) {

    /* Check if variable already exists */
    for (int i = 0; i < MAX_VARS; i++) {
        if (globallist[i].used && globallist[i].name == name) {
            globallist[i].variable = variable; // overwrite
            return true;
        }
    }

    /* Find free slot */
    for (int i = 0; i < MAX_VARS; i++) {
        if (!globallist[i].used) {
            globallist[i].used = true;
            globallist[i].name = name;
            globallist[i].variable = variable;
            return true;
        }
    }

    return false; // list full
}