#include "minimalos.h"

extern int main(void);

void _start(void) {
    int rc = main();
    mos_exit(rc);
}