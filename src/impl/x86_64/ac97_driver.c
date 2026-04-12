/*
 * AC97 Audio Driver (Intel ICH)
 * Simple driver to actually OUTPUT audio to hardware
 */

#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include "x86_64/ac97_driver.h"
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/port.h"
#include "x86_64/allocator.h"
#include "serial.h"

bool adebug = false;

// AC97 PCI IDs
#define AC97_VENDOR_INTEL  0x8086
#define AC97_DEVICE_ICH    0x2415

// PCI class codes for multimedia audio
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_AUDIO   0x01

// AC97 Registers (NABM - Native Audio Bus Master)
#define AC97_NABM_POBDBAR  0x10  // PCM Out Buffer Descriptor Base Address
#define AC97_NABM_CIV      0x14  // PCM Out Current Index Value
#define AC97_NABM_POLVI    0x15  // PCM Out Last Valid Index
#define AC97_NABM_POSR     0x16  // PCM Out Status Register
#define AC97_NABM_PICB     0x18  // PCM Out Position in Current Buffer
#define AC97_NABM_POCR     0x1B  // PCM Out Control Register

// AC97 Mixer (NAM) registers
#define AC97_NAM_RESET     0x00
#define AC97_NAM_MASTER    0x02
#define AC97_NAM_PCM_OUT   0x18
#define AC97_NAM_EXT_AUDIO_ID   0x28
#define AC97_NAM_EXT_AUDIO_CTRL 0x2A
#define AC97_NAM_PCM_FRONT_DAC_RATE 0x2C
#define AC97_NAM_VENDOR_ID1 0x7C
#define AC97_NAM_VENDOR_ID2 0x7E

// Buffer Descriptor
typedef struct {
    uint32_t buffer_addr;
    uint16_t length;     // Length in samples (not bytes!)
    uint16_t flags;
} __attribute__((packed)) ac97_bd_t;

#define AC97_BD_IOC  (1 << 15)  // Interrupt on Completion
#define AC97_BD_BUP  (1 << 14)  // Buffer Underrun Policy (repeat last sample)

#define AC97_BD_COUNT 32
#define AC97_FRAMES_PER_BUFFER AUDIO_BUFFER_SIZE
#define AC97_CHANNELS AUDIO_CHANNELS
#define AC97_BYTES_PER_FRAME (AC97_CHANNELS * sizeof(int16_t)) // interleaved samples
#define AC97_BUFFER_BYTES (AC97_FRAMES_PER_BUFFER * AC97_BYTES_PER_FRAME)

static uintptr_t g_nabm_base = 0;
static uintptr_t g_nam_base = 0;
static ac97_bd_t* g_bd_list = NULL;
static int16_t* g_audio_buffer_base = NULL;
static int16_t* g_audio_buffers[AC97_BD_COUNT] = {0};
static bool g_ac97_initialized = false;
static uint8_t g_next_fill = 0;
static uint8_t g_last_valid = 0;
static bool g_vra_supported = false;
static uint8_t g_target_ahead = 4;

static inline uint16_t ac97_mixer_read(uint16_t reg) {
    return port_inw((uint16_t)(g_nam_base + reg));
}

static inline void ac97_mixer_write(uint16_t reg, uint16_t val) {
    port_outw((uint16_t)(g_nam_base + reg), val);
}

bool ac97_init(void) {
    serial_write_str("AC97: Searching for audio controller...\n");

    if (pci_get_device_count() == 0) {
        pci_enumerate_all();
    }

    pci_device_t* dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH);
    if (!dev) {
        dev = pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO);
    }

    if (!dev) {
        serial_write_str("AC97: No compatible device found\n");
        return false;
    }

    pci_enable_io_busmaster(dev);

    g_nabm_base = dev->bar[1];
    g_nam_base  = dev->bar[0];

    if (!g_nabm_base || !g_nam_base) {
        serial_write_str("AC97: Invalid BARs\n");
        return false;
    }

    ac97_mixer_write(AC97_NAM_RESET, 0x0000);
    ac97_mixer_write(AC97_NAM_MASTER, 0x0000);
    ac97_mixer_write(AC97_NAM_PCM_OUT, 0x0000);

    g_bd_list = (ac97_bd_t*)kmalloc_aligned(
        AC97_BD_COUNT * sizeof(ac97_bd_t), 256);

    g_audio_buffer_base = (int16_t*)kmalloc_aligned(
        AC97_BD_COUNT * AC97_BUFFER_BYTES, 4096);

    if (!g_bd_list || !g_audio_buffer_base) {
        serial_write_str("AC97: alloc failed\n");
        return false;
    }

    // IMPORTANT FIX: correct sample count per buffer
    uint32_t samples_per_buffer =
        AC97_FRAMES_PER_BUFFER * AC97_CHANNELS;

    for (int i = 0; i < AC97_BD_COUNT; i++) {

        g_audio_buffers[i] =
            (int16_t*)((uint8_t*)g_audio_buffer_base +
            (i * AC97_BUFFER_BYTES));

        g_bd_list[i].buffer_addr =
            (uint32_t)(uintptr_t)g_audio_buffers[i];

        g_bd_list[i].length = (uint16_t)samples_per_buffer;

        // IMPORTANT: IOC causes interrupts → optional, but stable
        g_bd_list[i].flags = AC97_BD_BUP;
    }

    port_outl(g_nabm_base + AC97_NABM_POBDBAR,
              (uint32_t)(uintptr_t)g_bd_list);

    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1F);
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);

    g_last_valid = AC97_BD_COUNT - 1;
    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);

    // Prime buffers
    for (int i = 0; i < AC97_BD_COUNT; i++) {
        audio_mix_streams(g_audio_buffers[i], AC97_FRAMES_PER_BUFFER);
        g_last_valid = i;
        port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    }

    port_outb(g_nabm_base + AC97_NABM_POCR, 0x05);

    uint16_t ext_id = ac97_mixer_read(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x1) {
        g_vra_supported = true;
        ac97_mixer_write(AC97_NAM_EXT_AUDIO_CTRL, 0x1);
        ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, 48000);
    }

    g_ac97_initialized = true;
    serial_write_str("AC97: Initialized OK\n");
    return true;
}

// TODO: AC97 DMA / FIFO feeding - Improve buffer feeding strategy
// Called from timer interrupt or dedicated audio interrupt
void ac97_update(void) {
    if (!g_ac97_initialized) return;

    uint8_t civ = port_inb((uint16_t)(g_nabm_base + AC97_NABM_CIV));

    static uint8_t last_civ = 0xFF;
    static uint32_t stall = 0;

    if (civ == last_civ) stall++;
    else { stall = 0; last_civ = civ; }

    if (stall > 25) {
        ac97_kick();
        stall = 0;
        return;
    }

    uint8_t safety_gap = 2;

    for (uint8_t i = 0; i < g_target_ahead; i++) {
        uint8_t next = (g_last_valid + 1) % AC97_BD_COUNT;

        uint8_t dist = (next + AC97_BD_COUNT - civ) % AC97_BD_COUNT;
        if (dist <= safety_gap) break;

        // FIX: use audio_mix_streams like ac97_kick does,
        // instead of the broken g_audio_ring direct copy
        audio_mix_streams(g_audio_buffers[next], AC97_FRAMES_PER_BUFFER);

        g_last_valid = next;
        port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    }

    g_next_fill = (g_last_valid + 1) % AC97_BD_COUNT;
}

void ac97_kick(void) {
    if (!g_ac97_initialized) return;

    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1F);

    for (int i = 0; i < AC97_BD_COUNT; i++) {
        audio_mix_streams(g_audio_buffers[i], AC97_FRAMES_PER_BUFFER);
        g_last_valid = i;
    }

    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x05);
}

// TODO: correct sample rate propagation - Ensure sample rate changes sync with audio player timing
void ac97_set_sample_rate(uint32_t rate_hz) {
    if (!g_ac97_initialized) return;
    if (!g_vra_supported) return;
    if (rate_hz < 8000) rate_hz = 8000;
    if (rate_hz > 48000) rate_hz = 48000;
    ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, (uint16_t)rate_hz);
    serial_write_str("AC97: Set DAC rate=");
    serial_write_dec(rate_hz);
    serial_write_str("\n");
    // TODO: Propagate rate to player state for accurate buffer timing
}
