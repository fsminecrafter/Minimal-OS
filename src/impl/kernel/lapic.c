#include "x86_64/lapic.h"
#include "x86_64/acpi.h"
#include "x86_64/mmio.h"
#include "serial.h"

#define IA32_APIC_BASE_MSR       0x1B
#define IA32_APIC_BASE_ENABLE    (1ULL << 11)
#define LAPIC_SVR_ENABLE         (1U << 8)
#define LAPIC_ICR_DELIVERY_STATUS (1U << 12)

static volatile uint32_t* g_lapic_mmio;
static volatile uint32_t g_lapic_map_lock;

static inline uint64_t read_apic_base_msr(void) {
    uint32_t low;
    uint32_t high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_APIC_BASE_MSR));
    return ((uint64_t)high << 32) | low;
}

static inline void write_apic_base_msr(uint64_t value) {
    asm volatile("wrmsr"
                 :: "c"(IA32_APIC_BASE_MSR),
                    "a"((uint32_t)value),
                    "d"((uint32_t)(value >> 32)));
}

static inline uint32_t lapic_read(uint32_t reg) {
    return g_lapic_mmio[reg / sizeof(uint32_t)];
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    g_lapic_mmio[reg / sizeof(uint32_t)] = value;
}

static bool lapic_map(void) {
    if (g_lapic_mmio) return true;

    while (__sync_lock_test_and_set(&g_lapic_map_lock, 1)) {
        asm volatile("pause");
    }

    if (!g_lapic_mmio) {
        void* va = mmio_alloc_va(0x1000);
        uintptr_t phys = acpi_get_lapic_phys_addr();
        if (va && map_mmio_page((uintptr_t)va, phys, 0x1000)) {
            g_lapic_mmio = (volatile uint32_t*)va;
        }
    }

    __sync_lock_release(&g_lapic_map_lock);
    return g_lapic_mmio != NULL;
}

bool lapic_init(void) {
    if (!lapic_map()) {
        serial_write_str("LAPIC: failed to map MMIO region\n");
        return false;
    }

    uint64_t apic_base = read_apic_base_msr();
    write_apic_base_msr(apic_base | IA32_APIC_BASE_ENABLE);
    lapic_write(LAPIC_REG_SVR, LAPIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);
    lapic_write(LAPIC_REG_EOI, 0);
    return true;
}

uint32_t lapic_get_id(void) {
    return g_lapic_mmio ? (lapic_read(LAPIC_REG_ID) >> 24) : 0;
}

void lapic_send_eoi(void) {
    if (g_lapic_mmio) lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_wait_icr_idle(void) {
    if (!g_lapic_mmio) return;
    while (lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS) {
        asm volatile("pause");
    }
}

static void lapic_send_ipi(uint32_t target_apic_id, uint32_t delivery_mode, uint8_t vector) {
    if (!g_lapic_mmio) return;
    lapic_wait_icr_idle();
    lapic_write(LAPIC_REG_ICR_HIGH, target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, delivery_mode | vector | LAPIC_ICR_LEVEL_ASSERT);
    lapic_wait_icr_idle();
}

void lapic_send_init_ipi(uint32_t target_apic_id) {
    lapic_send_ipi(target_apic_id, LAPIC_ICR_DELIVERY_INIT, 0);
}

void lapic_send_startup_ipi(uint32_t target_apic_id, uint8_t vector) {
    lapic_send_ipi(target_apic_id, LAPIC_ICR_DELIVERY_STARTUP, vector);
}
