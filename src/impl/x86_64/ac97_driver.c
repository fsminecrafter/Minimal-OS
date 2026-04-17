/*
 * AC97 Audio Driver (Intel ICH)
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

#define AC97_VENDOR_INTEL  0x8086
#define AC97_DEVICE_ICH    0x2415

#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_AUDIO   0x01

/* NABM register offsets */
#define AC97_NABM_POBDBAR  0x10
#define AC97_NABM_CIV      0x14
#define AC97_NABM_POLVI    0x15
#define AC97_NABM_POSR     0x16
#define AC97_NABM_PICB     0x18
#define AC97_NABM_POCR     0x1B

/* NAM (mixer) register offsets */
#define AC97_NAM_RESET              0x00
#define AC97_NAM_MASTER             0x02
#define AC97_NAM_PCM_OUT            0x18
#define AC97_NAM_EXT_AUDIO_ID       0x28
#define AC97_NAM_EXT_AUDIO_CTRL     0x2A
#define AC97_NAM_PCM_FRONT_DAC_RATE 0x2C
#define AC97_NAM_VENDOR_ID1         0x7C
#define AC97_NAM_VENDOR_ID2         0x7E

/* POCR bits */
#define AC97_POCR_RPBM  (1 << 0)   /* Run/Pause Bus Master */
#define AC97_POCR_RR    (1 << 1)   /* Reset Registers */
#define AC97_POCR_LVBIE (1 << 2)   /* Last Valid Buffer Interrupt Enable */
#define AC97_POCR_FEIE  (1 << 3)   /* FIFO Error Interrupt Enable */
#define AC97_POCR_IOCE  (1 << 4)   /* Interrupt On Completion Enable */

/* Buffer Descriptor */
typedef struct {
    uint32_t buffer_addr;
    uint16_t length;   /* samples, NOT bytes */
    uint16_t flags;
} __attribute__((packed)) ac97_bd_t;

#define AC97_BD_IOC  (1 << 15)
#define AC97_BD_BUP  (1 << 14)

#define AC97_BD_COUNT        32
#define AC97_FRAMES_PER_BUFFER AUDIO_BUFFER_SIZE
#define AC97_CHANNELS        AUDIO_CHANNELS
#define AC97_BYTES_PER_FRAME (AC97_CHANNELS * sizeof(int16_t))
#define AC97_BUFFER_BYTES    (AC97_FRAMES_PER_BUFFER * AC97_BYTES_PER_FRAME)

static uintptr_t g_nabm_base = 0;
static uintptr_t g_nam_base  = 0;
static ac97_bd_t* g_bd_list  = NULL;
static int16_t*   g_audio_buffer_base = NULL;
static int16_t*   g_audio_buffers[AC97_BD_COUNT] = {0};
static bool  g_ac97_initialized = false;
static uint8_t g_last_valid  = 0;
static bool  g_vra_supported = false;
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

    /* Cold reset mixer */
    ac97_mixer_write(AC97_NAM_RESET, 0x0000);
    /* Unmute master and PCM at max volume */
    ac97_mixer_write(AC97_NAM_MASTER, 0x0000);
    ac97_mixer_write(AC97_NAM_PCM_OUT, 0x0000);

    /* Allocate descriptor list and sample buffers */
    g_bd_list = (ac97_bd_t*)kmalloc_aligned(
        AC97_BD_COUNT * sizeof(ac97_bd_t), 256);
    g_audio_buffer_base = (int16_t*)kmalloc_aligned(
        AC97_BD_COUNT * AC97_BUFFER_BYTES, 4096);

    if (!g_bd_list || !g_audio_buffer_base) {
        serial_write_str("AC97: alloc failed\n");
        return false;
    }

    /* samples_per_buffer = frames × channels (interleaved L/R) */
    uint32_t samples_per_buffer = AC97_FRAMES_PER_BUFFER * AC97_CHANNELS;

    for (int i = 0; i < AC97_BD_COUNT; i++) {
        g_audio_buffers[i] =
            (int16_t*)((uint8_t*)g_audio_buffer_base + i * AC97_BUFFER_BYTES);

        /* Zero-fill so silence plays before any real audio is queued */
        for (uint32_t s = 0; s < samples_per_buffer; s++) {
            g_audio_buffers[i][s] = 0;
        }

        g_bd_list[i].buffer_addr = (uint32_t)(uintptr_t)g_audio_buffers[i];
        g_bd_list[i].length      = (uint16_t)samples_per_buffer;
        /* BUP: repeat last sample on underrun (no IOC — polling only) */
        g_bd_list[i].flags       = AC97_BD_BUP;
    }

    /* Program descriptor base */
    port_outl(g_nabm_base + AC97_NABM_POBDBAR,
              (uint32_t)(uintptr_t)g_bd_list);

    /* Clear status, stop DMA */
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1C);
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);

    /* Start at buffer 0; last valid = 0 (just one silent buffer visible) */
    g_last_valid = 0;
    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);

    /* Check VRA support */
    uint16_t ext_id = ac97_mixer_read(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x1) {
        g_vra_supported = true;
        ac97_mixer_write(AC97_NAM_EXT_AUDIO_CTRL, 0x1);
        ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, 48000);
        serial_write_str("AC97: VRA supported, default rate 48000 Hz\n");
    } else {
        serial_write_str("AC97: No VRA, fixed 48000 Hz\n");
    }

    g_ac97_initialized = true;
    serial_write_str("AC97: Initialized (DMA not yet started — call ac97_start)\n");
    return true;
}

/*
 * ac97_start — prime buffers with real audio and start DMA.
 * Call this AFTER the player is set up and ready to provide samples,
 * so the first buffer presented to the hardware contains real audio.
 */
void ac97_start(void) {
    if (!g_ac97_initialized) return;

    serial_write_str("AC97: Starting DMA...\n");

    /* Stop any running DMA and clear status */
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1C);

    /* Reset last valid index */
    g_last_valid = 0;

    /* Prime all 32 buffers with real audio from the player */
    for (int i = 0; i < AC97_BD_COUNT; i++) {
        audio_mix_streams(g_audio_buffers[i], AC97_FRAMES_PER_BUFFER);
        g_last_valid = (uint8_t)i;
        port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    }

    /* Start DMA: RPBM bit */
    port_outb(g_nabm_base + AC97_NABM_POCR, AC97_POCR_RPBM);

    serial_write_str("AC97: DMA started\n");
}

void ac97_update(void) {
    if (!g_ac97_initialized) return;

    uint8_t civ = port_inb((uint16_t)(g_nabm_base + AC97_NABM_CIV));

    static uint8_t last_civ = 0xFF;
    static uint32_t stall   = 0;

    if (civ == last_civ) {
        stall++;
    } else {
        stall = 0;
        last_civ = civ;
    }

    if (stall > 25) {
        ac97_kick();
        stall = 0;
        return;
    }

    /*
     * Fill ahead g_target_ahead buffers beyond what the HW is currently
     * consuming, but keep a safety_gap so we never write INTO the buffer
     * the hardware is reading right now.
     */
    const uint8_t safety_gap = 2;

    for (uint8_t i = 0; i < g_target_ahead; i++) {
        uint8_t next = (g_last_valid + 1) % AC97_BD_COUNT;
        uint8_t dist = (uint8_t)((next + AC97_BD_COUNT - civ) % AC97_BD_COUNT);
        if (dist <= safety_gap) break;

        audio_mix_streams(g_audio_buffers[next], AC97_FRAMES_PER_BUFFER);
        g_last_valid = next;
        port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    }
}

void ac97_kick(void) {
    if (!g_ac97_initialized) return;

    serial_write_str("AC97: kick\n");

    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1C);

    g_last_valid = 0;
    for (int i = 0; i < AC97_BD_COUNT; i++) {
        audio_mix_streams(g_audio_buffers[i], AC97_FRAMES_PER_BUFFER);
        g_last_valid = (uint8_t)i;
    }

    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    port_outb(g_nabm_base + AC97_NABM_POCR, AC97_POCR_RPBM);
}

void ac97_set_sample_rate(uint32_t rate_hz) {
    if (!g_ac97_initialized) return;
    if (!g_vra_supported) {
        serial_write_str("AC97: VRA not supported, ignoring rate change\n");
        return;
    }
    if (rate_hz < 8000)  rate_hz = 8000;
    if (rate_hz > 48000) rate_hz = 48000;
    ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, (uint16_t)rate_hz);
    serial_write_str("AC97: DAC rate=");
    serial_write_dec(rate_hz);
    serial_write_str(" Hz\n");
}
