// src/impl/kernel/smp.c
#include "x86_64/smp.h"
#include "x86_64/acpi.h"
#include "x86_64/lapic.h"
#include "x86_64/lapic_timer.h"
#include "x86_64/ioapic.h"
#include "x86_64/ap_trampoline.h"
#include "x86_64/gdt.h"
#include "x86_64/idt.h"
#include "x86_64/pit.h"
#include "x86_64/pmm.h"
#include "serial.h"
#include "string.h"
#include "time.h"

#include "panic.h"

cpu_local_t g_cpus[MAX_CPUS];
volatile uint32_t g_cpu_count = 1;
volatile uint32_t g_online_cpu_count = 1;

static uint32_t g_apic_id_to_cpu[MAX_CPUS];
static uint32_t g_bsp_apic_id = 0;
static volatile bool g_bsp_lapic_ready = false;

typedef struct __attribute__((packed)) {
    uint64_t pml4_phys;
    uint64_t stack_top;
    uint64_t entry_addr;
    uint32_t cpu_id;
} ap_mailbox_t;

#define AP_MAILBOX_SIZE 28   // must match ap_trampoline.asm's mailbox_* layout exactly

extern uint64_t pml4_phys_addr;   // from main.asm, same extern proc.c already uses
extern struct IdtPtr idt_ptr;     // from idt.c - shared across every core

/*
 * Previously always returned 0. With real APs, "which core am I" has
 * to come from actual hardware (LAPIC ID is per-core banked hardware,
 * safe to read from any core), mapped back to our logical cpu_id via
 * the table smp_start_aps() filled in. O(MAX_CPUS) per call - fine at
 * 16 cores, but would want a %gs-relative per-core variable if this
 * ever shows up hot.
 */
uint32_t smp_current_cpu_id(void) {
    if (!g_bsp_lapic_ready) return SMP_MASTER_CPU_ID;

    uint32_t apic_id = lapic_get_id();
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_apic_id_to_cpu[i] == apic_id) return i;
    }
    return 0;
}

uint32_t smp_online_cpu_count(void) {
    return __atomic_load_n(&g_online_cpu_count, __ATOMIC_ACQUIRE);
}

void smp_get_cpu_usage(uint32_t cpu_id, uint32_t* average, uint32_t* usage) {
    if (cpu_id >= MAX_CPUS) return;

    uint64_t total = __atomic_load_n(&g_cpus[cpu_id].usage_total_ticks,
                                     __ATOMIC_RELAXED);
    uint64_t busy = __atomic_load_n(&g_cpus[cpu_id].usage_busy_ticks,
                                    __ATOMIC_RELAXED);
    uint32_t recent = __atomic_load_n(&g_cpus[cpu_id].usage_last_percent,
                                      __ATOMIC_RELAXED);

    if (average) *average = total ? (uint32_t)((busy * 100) / total) : 0;
    if (usage) *usage = recent;
}

void smp_init_bsp(void) {
    g_bsp_lapic_ready = false;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        g_cpus[i] = (cpu_local_t){ .cpu_id = i };
        g_apic_id_to_cpu[i] = 0;
    }
    g_cpus[0].online = true;
    g_cpu_count = 1;
    g_online_cpu_count = 1;

    serial_write_str("SMP: BSP online (cpu 0). Call smp_start_aps() once the "
                      "PIT/ACPI are ready to bring up the rest.\n");
}

static void busy_wait_ms(uint32_t ms) {
    uint64_t start = time_get_uptime_ms();
    while ((time_get_uptime_ms() - start) < ms) asm volatile("pause");
}

void smp_assert_master_core(const char* subsystem) {
    if (smp_is_master_core()) return;

    serial_write_str("SMP: FATAL - '");
    serial_write_str(subsystem ? subsystem : "(unknown)");
    serial_write_str("' ran on non-master core ");
    serial_write_dec(smp_current_cpu_id());
    serial_write_str(" (master is cpu ");
    serial_write_dec((uint32_t)SMP_MASTER_CPU_ID);
    serial_write_str(") - this subsystem shares unlocked global state "
                      "and is not yet safe for multi-core delivery. "
                      "This means an IRQ got routed somewhere it "
                      "shouldn't have (e.g. a stray IOAPIC redirection "
                      "entry) - fix the routing, do not remove this "
                      "check.\n");
    PANIC("legacy IRQ subsystem ran off the master core");
}

static void busy_wait_us_approx(uint32_t us) {
    // No microsecond timer wired up yet (PIT tick is ~1ms via
    // pit_init(100) in startroutine.c). This is a calibration-free
    // spin, not a real microsecond delay - fine here since the MP
    // spec's ~200us gap between SIPIs is a conservative legacy figure;
    // being longer than needed is harmless, only being too short
    // (near-zero) actually risks the second SIPI arriving before the
    // AP has processed the first.
    for (volatile uint32_t i = 0; i < us * 200u; i++) asm volatile("nop");
}

uint32_t smp_start_aps(multiboot2_info_t* mb_info) {
    if (!acpi_init(mb_info)) {
        serial_write_str("SMP: ACPI unavailable - staying single-core\n");
        return g_cpu_count;
    }

    if (!lapic_init()) {
        serial_write_str("SMP: LAPIC init failed - staying single-core\n");
        return g_cpu_count;
    }
    g_bsp_lapic_ready = true;
    g_bsp_apic_id        = lapic_get_id();
    g_apic_id_to_cpu[0]  = g_bsp_apic_id;

    // Mapped and masked if present, but existing PIC-routed IRQs
    // (PIT/keyboard) are deliberately left alone here - moving them
    // over means reprogramming idt_init()'s vectors, masking the PIC,
    // and deciding which core(s) each IRQ should target. Not required
    // just to get extra cores executing code; see the note in
    // ap_entry_c() below about what that means for those cores today.
    if (acpi_get_ioapic_count() > 0) {
        ioapic_init(acpi_get_ioapic(0)->phys_addr);
    }

    size_t trampoline_size = (size_t)(_binary_ap_trampoline_bin_end - _binary_ap_trampoline_bin_start);
    if (trampoline_size == 0 || trampoline_size > 0x1000 - AP_MAILBOX_SIZE) {
        serial_write_str("SMP: trampoline blob missing or too large for its page\n");
        return g_cpu_count;
    }

    // Low memory is identity-mapped (page_table_l2's 2MB pages), so
    // this physical load address is directly writable right here.
    memcpy((void*)AP_TRAMPOLINE_ADDR, _binary_ap_trampoline_bin_start, trampoline_size);

    ap_mailbox_t* mailbox = (ap_mailbox_t*)(AP_TRAMPOLINE_ADDR + trampoline_size - AP_MAILBOX_SIZE);
    mailbox->pml4_phys  = pml4_phys_addr;
    mailbox->entry_addr = (uint64_t)(uintptr_t)ap_entry_c;

    uint32_t cpu_total = acpi_get_cpu_count();
    uint32_t started = 1; // BSP

    for (uint32_t i = 0; i < cpu_total && started < MAX_CPUS; i++) {
        const acpi_cpu_t* cpu = acpi_get_cpu(i);
        if (!cpu || cpu->apic_id == g_bsp_apic_id) continue;

        uint32_t cpu_id = started;

        void* stack = alloc_pages_zeroed(4); // 16KB AP kernel stack
        if (!stack) {
            serial_write_str("SMP: OOM allocating AP stack, stopping bring-up\n");
            break;
        }

        mailbox->stack_top = (uint64_t)(uintptr_t)stack + 4 * 0x1000;
        mailbox->cpu_id    = cpu_id;

        g_apic_id_to_cpu[cpu_id] = cpu->apic_id;
        g_cpus[cpu_id].lapic_id  = cpu->apic_id;
        g_cpus[cpu_id].online    = false;

        serial_write_str("SMP: starting AP apic_id=");
        serial_write_dec(cpu->apic_id);
        serial_write_str(" as cpu_id=");
        serial_write_dec(cpu_id);
        serial_write_str("\n");

        lapic_send_init_ipi(cpu->apic_id);
        busy_wait_ms(10);

        uint8_t vector = (uint8_t)(AP_TRAMPOLINE_ADDR >> 12);
        lapic_send_startup_ipi(cpu->apic_id, vector);
        busy_wait_us_approx(200);
        lapic_send_startup_ipi(cpu->apic_id, vector);

        uint32_t waited_ms = 0;
        while (!g_cpus[cpu_id].online && waited_ms < 200) {
            busy_wait_ms(1);
            waited_ms++;
        }

        if (g_cpus[cpu_id].online) {
            started++;
            g_cpu_count = started;
        } else {
            serial_write_str("SMP: AP did not come online (timeout)\n");
            free_pages(stack, 4);
        }
    }

    serial_write_str("SMP: ");
    serial_write_dec(g_cpu_count);
    serial_write_str(" core(s) online\n");
    return g_cpu_count;
}

void ap_entry_c(uint32_t cpu_id) __attribute__((__noreturn__));
void ap_entry_c(uint32_t cpu_id) {
    gdt_load_this_cpu(cpu_id);
    idt_load(&idt_ptr);   // shares the BSP's single IDT - fine, entries only encode handler addr/selector
    lapic_init();

    g_cpus[cpu_id].current_process = NULL;
    g_cpus[cpu_id].idle_process    = NULL;

    g_cpus[cpu_id].online = true;
    __atomic_fetch_add(&g_online_cpu_count, 1, __ATOMIC_RELEASE);

    serial_write_str("SMP: cpu online\n");

    /*
     * Start this core's own periodic tick so scheduler_tick() actually
     * runs here. Previously this core just sat in `hlt` forever with
     * interrupts still disabled from the trampoline's initial `cli` -
     * nothing, timer or otherwise, could ever wake it, so the per-CPU
     * run queue it was just given a slot in (see
     * scheduler_enqueue_new()/scheduler_rebalance() in scheduler.c)
     * had no way to ever get drained.
     *
     * Calibrated against the wall clock (time_get_uptime_ms(), itself
     * driven by the master core's PIT interrupt - see time_tick() in
     * time.c) so this core's scheduler tick fires at the same target
     * frequency the BSP uses (pit_get_frequency()), rather than at
     * whatever rate an uncalibrated fixed initial count happens to
     * produce on this CPU's actual bus clock. This matters because
     * MLFQ quantum lengths (sched_level_quantum_ticks in scheduler.c)
     * are expressed in tick counts: without calibration, two cores
     * "using the same quantum" in tick terms could be running wildly
     * different quanta in wall-clock terms, which would make the
     * load figures scheduler_rebalance() compares across cores
     * meaningless.
     *
     * By the time any AP reaches here, the PIT has been ticking (and
     * the BSP has had interrupts enabled) since startroutine() ran
     * early in kernel_main() - see idt_init()'s trailing sti() - so
     * time_get_uptime_ms() is a valid live reference. If calibration
     * still fails for some reason, lapic_timer_init_calibrated() falls
     * back to the old fixed placeholder count rather than leaving the
     * timer unprogrammed - see its comment in lapic_timer.c.
     */
    lapic_timer_init_calibrated(LAPIC_TIMER_VECTOR, pit_get_frequency(), 3 /* divide by 16 */);
    asm volatile("sti" ::: "memory");

    // Idle here just means "nothing on this core's run queue right
    // now" - hlt lets the next timer interrupt (or a future rebalance
    // donating this core a process) wake it instead of spinning.
    for (;;) asm volatile("hlt");
}