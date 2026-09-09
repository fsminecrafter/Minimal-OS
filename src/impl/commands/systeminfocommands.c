#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "serial.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/globaldatatable.h"
#include "x86_64/pci.h"
#include "x86_64/smp.h"

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* eax,
                  uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

static uint32_t cpuid_max_leaf(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

static uint32_t cpuid_max_extended_leaf(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

static void get_cpu_name(char* name) {
    if (cpuid_max_extended_leaf() < 0x80000004) {
        name[0] = 'U';
        name[1] = 'n';
        name[2] = 'k';
        name[3] = 'n';
        name[4] = 'o';
        name[5] = 'w';
        name[6] = 'n';
        name[7] = '\0';
        return;
    }

    uint32_t* words = (uint32_t*)name;
    for (uint32_t leaf = 0; leaf < 3; leaf++) {
        uint32_t eax;
        uint32_t ebx;
        uint32_t ecx;
        uint32_t edx;
        cpuid(0x80000002 + leaf, 0, &eax, &ebx, &ecx, &edx);
        words[leaf * 4 + 0] = eax;
        words[leaf * 4 + 1] = ebx;
        words[leaf * 4 + 2] = ecx;
        words[leaf * 4 + 3] = edx;
    }
    name[48] = '\0';
}

static void info_text(const char* text) {
    graphics_write_textr(text);
    serial_write_str(text);
}

static void info_number(uint64_t value) {
    graphics_write_textr_udec(value);
    serial_write_dec(value);
}

static void info_hex(uint64_t value) {
    graphics_write_textr_hex(value);
    serial_write_hex(value);
}

void cmd_clear(int argc, const char** argv) {
    graphics_terminal_clear();
    serial_write_str("clear: done\n");
}

void cmd_info(int argc, const char** argv) {
    char cpu_name[49];
    get_cpu_name(cpu_name);

    info_text("CPU name: ");
    info_text(cpu_name);
    info_text("\nCore amount: ");
    info_number(g_cpu_count);
    info_text("\nCPU speed: ");

    uint32_t cpu_speed_mhz = 0;
    if (cpuid_max_leaf() >= 0x16) {
        uint32_t eax;
        uint32_t ebx;
        uint32_t ecx;
        uint32_t edx;
        cpuid(0x16, 0, &eax, &ebx, &ecx, &edx);
        cpu_speed_mhz = eax;
    }
    if (cpu_speed_mhz == 0) {
        info_text("Unknown");
    } else {
        info_number(cpu_speed_mhz);
        info_text(" MHz");
    }

    info_text("\nPCI devices:\n");
    for (int i = 0; i < pci_device_count; i++) {
        info_text(" device ");
        info_number((uint64_t)i + 1);
        info_text(" at ");
        info_hex(pci_devices[i].device_id);
        info_text(" (bus ");
        info_number(pci_devices[i].bus);
        info_text(", slot ");
        info_number(pci_devices[i].device);
        info_text(", function ");
        info_number(pci_devices[i].function);
        info_text(")\n");
    }

    info_text("RAM amount: ");
    info_number(findvar("totalrambytes") / (1024 * 1024));
    info_text(" MiB\nRAM speed: Unknown\n");
}

void register_system_info_commands(void) {
    command_register("clear", cmd_clear);
    command_register("info", cmd_info);
}

REGISTER_COMMAND(register_system_info_commands);