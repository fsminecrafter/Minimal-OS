#include "x86_64/smp.h"
#include "serial.h"

cpu_local_t g_cpus[MAX_CPUS];
volatile uint32_t g_cpu_count = 1;

uint32_t smp_current_cpu_id(void) {
    return 0; // BSP only until Phase 3
}

void smp_init_bsp(void) {
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        g_cpus[i].cpu_id           = i;
        g_cpus[i].lapic_id         = 0;
        g_cpus[i].online           = false;
        g_cpus[i].current_process  = NULL;
        g_cpus[i].idle_process     = NULL;
    }
    g_cpus[0].online = true;
    g_cpu_count = 1;

    serial_write_str("SMP: BSP online (cpu 0). AP bring-up not implemented yet.\n");
}