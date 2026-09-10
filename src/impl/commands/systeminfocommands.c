#include <stdint.h>
#include <stdbool.h>
#include "string.h"
#include "graphics.h"
#include "serial.h"
#include "x86_64/commandhandler.h"
#include "x86_64/commandreg.h"
#include "x86_64/globaldatatable.h"
#include "x86_64/pci.h"
#include "x86_64/smp.h"
#include "x86_64/gpu.h"
#include "x86_64/scheduler.h"
#include "x86_64/minimafs.h"
#include "time.h"

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

static uint64_t read_tsc(void) {
    uint32_t low;
    uint32_t high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static uint32_t cpu_speed_mhz(void) {
    uint32_t max_leaf = cpuid_max_leaf();
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    if (max_leaf >= 0x16) {
        cpuid(0x16, 0, &eax, &ebx, &ecx, &edx);
        if (eax != 0) return eax;
    }

    if (max_leaf >= 0x15) {
        cpuid(0x15, 0, &eax, &ebx, &ecx, &edx);
        if (eax != 0 && ebx != 0 && ecx != 0) {
            return (uint32_t)(((uint64_t)ecx * ebx) / eax / 1000000ULL);
        }
    }

    uint64_t start_tsc = read_tsc();
    uint64_t start_ms = time_get_uptime_ms();
    sleep(100);
    uint64_t elapsed_ms = time_get_uptime_ms() - start_ms;
    uint64_t elapsed_tsc = read_tsc() - start_tsc;
    if (elapsed_ms == 0) return 0;
    return (uint32_t)(elapsed_tsc / elapsed_ms / 1000ULL);
}

static void get_cpu_vendor(char* vendor) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';
}

static void print_pci_location(const pci_device_t* dev) {
    info_number(dev->bus);
    info_text(":");
    info_number(dev->device);
    info_text(".");
    info_number(dev->function);
}

static const char* find_system_conf(void) {
    static const char* paths[] = {
        "0:/Etc/System.conf",
        "0:/etc/system.conf",
        "1:/Etc/System.conf",
        "1:/etc/system.conf"
    };
    for (uint32_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        minimafs_file_handle_t* file = minimafs_open(paths[i], true);
        if (file) {
            minimafs_close(file);
            return paths[i];
        }
    }
    return NULL;
}

static bool write_fixed_config_value(const char* path, const char* key, uint32_t value) {
    minimafs_file_handle_t* file = minimafs_open(path, false);
    if (!file) return false;

    char value_text[6];
    value_text[0] = '0' + (value / 10000) % 10;
    value_text[1] = '0' + (value / 1000) % 10;
    value_text[2] = '0' + (value / 100) % 10;
    value_text[3] = '0' + (value / 10) % 10;
    value_text[4] = '0' + value % 10;
    value_text[5] = '\0';

    int32_t offset = findinfile(key, path);
    if (offset >= 0) {
        minimafs_seek(file, (uint32_t)offset + strlen(key) + 1);
        bool ok = minimafs_write(file, value_text, 5) == 5;
        minimafs_close(file);
        return ok;
    }

    if (file->data_size > 0 && file->data[file->data_size - 1] != '\n') {
        if (minimafs_seek(file, file->data_size) && minimafs_write(file, "\n", 1) != 1) {
            minimafs_close(file);
            return false;
        }
    } else {
        minimafs_seek(file, file->data_size);
    }

    char line[32];
    int length = snprintf(line, sizeof(line), "%s=%s\n", key, value_text);
    bool ok = length > 0 && minimafs_write(file, line, (uint32_t)length) == (uint32_t)length;
    minimafs_close(file);
    return ok;
}

static bool read_config_dimension(const char* path, const char* key, uint32_t* value) {
    char text[16];
    if (!minimafs_get_config_value(key, path, text, sizeof(text))) return false;
    uint32_t parsed = 0;
    for (const char* p = text; *p >= '0' && *p <= '9'; p++) {
        parsed = parsed * 10 + (uint32_t)(*p - '0');
    }
    if (parsed == 0 || parsed > 8192) return false;
    *value = parsed;
    return true;
}

bool systeminfo_load_saved_resolution(void) {
    const char* path = find_system_conf();
    uint32_t width = GPU_DEFAULT_WIDTH;
    uint32_t height = GPU_DEFAULT_HEIGHT;
    if (!path) {
        return false;
    }

    bool has_width = read_config_dimension(path, "DisplayWidth", &width);
    bool has_height = read_config_dimension(path, "DisplayHeight", &height);
    if (!has_width) width = GPU_DEFAULT_WIDTH;
    if (!has_height) height = GPU_DEFAULT_HEIGHT;

    if (!gpu_set_resolution(&gpu, width, height)) return false;
    graphics_set_resolution(width / 8, height / 8);

    if (!has_width) write_fixed_config_value(path, "DisplayWidth", width);
    if (!has_height) write_fixed_config_value(path, "DisplayHeight", height);
    return true;
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
    info_number(smp_online_cpu_count());
    info_text("\nCPU speed: ");

    uint32_t speed = cpu_speed_mhz();
    if (speed == 0) {
        info_text("Unavailable");
    } else {
        info_number(speed);
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
    info_text(" MiB\nRAM speed: Not reported by firmware\n");
}

void cmd_cpu(int argc, const char** argv) {
    char vendor[13];
    char name[49];
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    get_cpu_vendor(vendor);
    get_cpu_name(name);
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    uint32_t base_family = (eax >> 8) & 0xF;
    uint32_t base_model = (eax >> 4) & 0xF;
    uint32_t extended_family = (eax >> 20) & 0xFF;
    uint32_t extended_model = (eax >> 16) & 0xF;
    uint32_t family = base_family == 0xF ? base_family + extended_family : base_family;
    uint32_t model = (base_family == 0x6 || base_family == 0xF)
                   ? (extended_model << 4) | base_model : base_model;

    info_text("CPU vendor: ");
    info_text(vendor);
    info_text("\nCPU name: ");
    info_text(name);
    info_text("\nFamily: ");
    info_number(family);
    info_text("  Model: ");
    info_number(model);
    info_text("  Stepping: ");
    info_number(eax & 0xF);
    info_text("\nOnline cores: ");
    info_number(smp_online_cpu_count());
    info_text("\nLogical processors reported by CPUID: ");
    info_number((ebx >> 16) & 0xFF);
    info_text("\nFrequency: ");
    uint32_t speed = cpu_speed_mhz();
    if (speed == 0) info_text("Unavailable");
    else {
        info_number(speed);
        info_text(" MHz");
    }
    info_text("\nFeatures: ");
    if (edx & (1U << 9)) info_text("APIC ");
    if (edx & (1U << 25)) info_text("SSE ");
    if (edx & (1U << 26)) info_text("SSE2 ");
    if (ecx & (1U << 28)) info_text("AVX ");
    if (ecx & (1U << 21)) info_text("x2APIC ");
    info_text("\n");
}

void cmd_gpu(int argc, const char** argv) {
    uint32_t display_count = 0;
    for (int i = 0; i < pci_device_count; i++) {
        const pci_device_t* dev = &pci_devices[i];
        if (dev->class_code != 0x03) continue;
        display_count++;
        info_text("GPU ");
        info_number(display_count);
        info_text(" at ");
        print_pci_location(dev);
        info_text("\n  Vendor ID: 0x");
        info_hex(dev->vendor_id);
        info_text("\n  Device ID: 0x");
        info_hex(dev->device_id);
        info_text("\n  Class: 0x");
        info_hex(((uint64_t)dev->class_code << 16) | ((uint64_t)dev->subclass << 8) | dev->prog_if);
        info_text("\n  BARs:");
        for (int bar = 0; bar < PCI_NUM_BARS; bar++) {
            if (dev->bar_type[bar] == PCI_BAR_UNUSED) continue;
            info_text("\n    BAR");
            info_number(bar);
            info_text(" = 0x");
            info_hex(dev->bar[bar]);
            info_text(dev->bar_type[bar] == PCI_BAR_IO ? " (I/O)" : " (MMIO)");
        }
    }

    info_text("Display controllers: ");
    info_number(display_count);
    info_text("\n");

    if (display_count == 0) {
        info_text("No PCI display controller found.\n");
        return;
    }

    info_text("Active framebuffer: ");
    if (!gpu.pci_dev) {
        info_text("not initialized\n");
        return;
    }
    info_text(" ");
    info_number(gpu.width);
    info_text("x");
    info_number(gpu.height);
    info_text(" at ");
    info_number(gpu.bpp * 8);
    info_text(" bpp, pitch ");
    info_number(gpu.pitch);
    info_text(" bytes\n");
}

void cmd_cgres(int argc, const char** argv) {
    if (argc != 3) {
        info_text("Usage: cgres <width> <height>\n");
        return;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    for (const char* p = argv[1]; *p >= '0' && *p <= '9'; p++) width = width * 10 + (*p - '0');
    for (const char* p = argv[2]; *p >= '0' && *p <= '9'; p++) height = height * 10 + (*p - '0');
    if (width < 320 || height < 200 || width > 8192 || height > 8192) {
        info_text("cgres: resolution must be between 320x200 and 8192x8192\n");
        return;
    }
    if (!gpu.pci_dev || !gpu_set_resolution(&gpu, width, height)) {
        info_text("cgres: no initialized GPU\n");
        return;
    }
    graphics_set_resolution(width / 8, height / 8);

    const char* path = find_system_conf();
    if (path && write_fixed_config_value(path, "DisplayWidth", width) &&
        write_fixed_config_value(path, "DisplayHeight", height)) {
        info_text("Resolution changed and saved: ");
    } else {
        info_text("Resolution changed (no valid System.conf found): ");
    }
    info_number(width);
    info_text("x");
    info_number(height);
    info_text("\n");
}

// Get CPU TSC per core (simple metric for utilization tracking)
void cmd_usage(int argc, const char** argv) {
    info_text("CPU utilisation\n");
    info_text("=============================\n");

    uint32_t online_cpus = smp_online_cpu_count();
    if (online_cpus == 0) {
        info_text("No online CPUs detected\n");
        return;
    }

    for (uint32_t i = 0; i < online_cpus && i < MAX_CPUS; i++) {
        if (!g_cpus[i].online) continue;

        uint32_t average = 0;
        uint32_t usage = 0;
        smp_get_cpu_usage(i, &average, &usage);

        info_text("Core ");
        info_number(i + 1);
        info_text(" ");
        info_number(average);
        info_text("% avg ");
        info_number(usage);
        info_text("% usg");
        if (smp_is_master_core_id(i)) info_text(" (Master)");
        info_text("\n");
    }
}

void register_system_info_commands(void) {
    command_register("clear", cmd_clear);
    command_register("info", cmd_info);
    command_register("cpu", cmd_cpu);
    command_register("gpu", cmd_gpu);
    command_register("chgres", cmd_cgres);
    command_register("usage", cmd_usage);
}

REGISTER_COMMAND(register_system_info_commands);