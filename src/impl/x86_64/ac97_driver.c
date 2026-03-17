/*
 * AC97 Audio Driver (Intel ICH)
 * Simple driver to actually OUTPUT audio to hardware
 */

#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include "x86_64/pci.h"
#include "x86_64/mmio.h"
#include "x86_64/port.h"
#include "x86_64/allocator.h"
#include "serial.h"

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

    // Find AC97 device on PCI bus
    pci_device_t* dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH);
    if (!dev) {
        dev = pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO);
    }
    
    if (!dev) {
        serial_write_str("AC97: No compatible device found\n");
        return false;
    }

    pci_enable_io_busmaster(dev);
    
    // Get NABM base address from BAR1
    if (dev->bar_type[1] != PCI_BAR_IO || dev->bar[1] == 0) {
        serial_write_str("AC97: Invalid NABM BAR\n");
        return false;
    }

    g_nabm_base = dev->bar[1];

    // Get NAM (mixer) base address from BAR0
    if (dev->bar_type[0] != PCI_BAR_IO || dev->bar[0] == 0) {
        serial_write_str("AC97: Invalid NAM BAR\n");
        return false;
    }

    g_nam_base = dev->bar[0];
    
    serial_write_str("AC97: NABM Base = 0x");
    serial_write_hex(g_nabm_base);
    serial_write_str("\n");
    serial_write_str("AC97: NAM  Base = 0x");
    serial_write_hex(g_nam_base);
    serial_write_str("\n");

    uint16_t pci_cmd = pci_config_read_word(dev->bus, dev->device, dev->function, 0x04);
    serial_write_str("AC97: PCI CMD = 0x");
    serial_write_hex(pci_cmd);
    serial_write_str(" (IO+BM expected)\n");

    // Reset codec and set volumes (unmute)
    ac97_mixer_write(AC97_NAM_RESET, 0x0000);
    ac97_mixer_write(AC97_NAM_MASTER, 0x0000);  // 0 dB, unmuted
    ac97_mixer_write(AC97_NAM_PCM_OUT, 0x0000); // 0 dB, unmuted

    uint16_t vid1 = ac97_mixer_read(AC97_NAM_VENDOR_ID1);
    uint16_t vid2 = ac97_mixer_read(AC97_NAM_VENDOR_ID2);
    serial_write_str("AC97: Codec Vendor ID = 0x");
    serial_write_hex(((uint32_t)vid1 << 16) | vid2);
    serial_write_str("\n");
    
    // Allocate Buffer Descriptor List (32 entries, 256-byte aligned)
    g_bd_list = (ac97_bd_t*)kmalloc_aligned(AC97_BD_COUNT * sizeof(ac97_bd_t), 256);
    
    // Allocate audio buffers (ring of 32 buffers)
    g_audio_buffer_base = (int16_t*)kmalloc_aligned(AC97_BD_COUNT * AC97_BUFFER_BYTES, 4096);
    
    // Setup buffer descriptors
    for (int i = 0; i < AC97_BD_COUNT; i++) {
        g_audio_buffers[i] = (int16_t*)((uint8_t*)g_audio_buffer_base + (i * AC97_BUFFER_BYTES));
        g_bd_list[i].buffer_addr = (uint32_t)(uintptr_t)g_audio_buffers[i];
        g_bd_list[i].length = (uint16_t)(AC97_FRAMES_PER_BUFFER * AC97_CHANNELS);
        g_bd_list[i].flags = AC97_BD_IOC | AC97_BD_BUP;  // IOC + underrun policy
    }
    
    // Write BD list address to controller
    port_outl(g_nabm_base + AC97_NABM_POBDBAR, (uint32_t)(uintptr_t)g_bd_list);
    
    // Stop PCM out, clear status
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1F); // write-1-to-clear

    // Set Last Valid Index
    g_last_valid = AC97_BD_COUNT - 1;
    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    
    // Start playback
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x05);  // Run + IOC interrupt enable

    serial_write_str("AC97: BDBAR=0x");
    serial_write_hex(port_inl(g_nabm_base + AC97_NABM_POBDBAR));
    serial_write_str(" LVI=");
    serial_write_dec(port_inb(g_nabm_base + AC97_NABM_POLVI));
    serial_write_str(" SR=0x");
    serial_write_hex(port_inb(g_nabm_base + AC97_NABM_POSR));
    serial_write_str(" CR=0x");
    serial_write_hex(port_inb(g_nabm_base + AC97_NABM_POCR));
    serial_write_str("\n");

    serial_write_str("AC97: BD list ptr=");
    serial_write_hex((uintptr_t)g_bd_list);
    serial_write_str(" align=");
    serial_write_dec((uintptr_t)g_bd_list & 0xFF);
    serial_write_str(" buf base=");
    serial_write_hex((uintptr_t)g_audio_buffer_base);
    serial_write_str("\n");
    serial_write_str("AC97: BD0 addr=0x");
    serial_write_hex(g_bd_list[0].buffer_addr);
    serial_write_str(" len=");
    serial_write_dec(g_bd_list[0].length);
    serial_write_str(" flags=0x");
    serial_write_hex(g_bd_list[0].flags);
    serial_write_str("\n");

    // Enable variable rate audio if supported and set to 48 kHz (AC97 native rate)
    uint16_t ext_id = ac97_mixer_read(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001) {
        g_vra_supported = true;
        uint16_t ext_ctrl = ac97_mixer_read(AC97_NAM_EXT_AUDIO_CTRL);
        ac97_mixer_write(AC97_NAM_EXT_AUDIO_CTRL, ext_ctrl | 0x0001);
        ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, 48000);
        serial_write_str("AC97: VRA enabled, rate=48000\n");
    } else {
        g_vra_supported = false;
        serial_write_str("AC97: VRA not supported, using 48kHz fixed\n");
    }

    // Pre-fill all buffers before starting playback
    for (int i = 0; i < AC97_BD_COUNT; i++) {
        audio_mix_streams(g_audio_buffers[i], AC97_FRAMES_PER_BUFFER);
        g_last_valid = (uint8_t)i;
        port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    }
    g_next_fill = 0;
    
    g_ac97_initialized = true;
    serial_write_str("AC97: Initialized and playing\n");
    return true;
}

// Called from timer interrupt or dedicated audio interrupt
void ac97_update(void) {
    if (!g_ac97_initialized) return;
    
    // Keep a few buffers queued ahead of the current index to avoid underruns
    uint8_t civ = port_inb((uint16_t)(g_nabm_base + AC97_NABM_CIV));
    static uint8_t last_civ = 0xFF;
    static uint32_t stall_ticks = 0;

    if (civ == last_civ) {
        stall_ticks++;
    } else {
        stall_ticks = 0;
        last_civ = civ;
    }

    if (stall_ticks > 25) { // ~0.25s at 100Hz
        serial_write_str("AC97: DMA stall detected, re-priming\n");
        ac97_kick();
        stall_ticks = 0;
    }

    uint8_t queued = (g_last_valid + AC97_BD_COUNT - civ) % AC97_BD_COUNT;
    uint8_t next = (g_last_valid + 1) % AC97_BD_COUNT;
    while (queued < g_target_ahead && next != civ) {
        audio_mix_streams(g_audio_buffers[next], AC97_FRAMES_PER_BUFFER);
        g_last_valid = next;
        port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
        queued++;
        next = (g_last_valid + 1) % AC97_BD_COUNT;
    }
    g_next_fill = next;
    
    static uint32_t debug_tick = 0;
    debug_tick++;
    if ((debug_tick % 100) == 0) {
        uint8_t civ_dbg = port_inb((uint16_t)(g_nabm_base + AC97_NABM_CIV));
        uint8_t lvi = port_inb((uint16_t)(g_nabm_base + AC97_NABM_POLVI));
        uint16_t picb = port_inw((uint16_t)(g_nabm_base + AC97_NABM_PICB));
        uint8_t sr = port_inb((uint16_t)(g_nabm_base + AC97_NABM_POSR));
        uint8_t cr = port_inb((uint16_t)(g_nabm_base + AC97_NABM_POCR));

        int32_t energy = 0;
        for (int i = 0; i < 64; i++) {
            int16_t s = g_audio_buffers[civ_dbg][i];
            energy += (s < 0) ? -s : s;
        }

        serial_write_str("AC97: DMA civ=");
        serial_write_dec(civ_dbg);
        serial_write_str(" lvi=");
        serial_write_dec(lvi);
        serial_write_str(" picb=");
        serial_write_dec(picb);
        serial_write_str(" sr=0x");
        serial_write_hex(sr);
        serial_write_str(" cr=0x");
        serial_write_hex(cr);
        serial_write_str(" next=");
        serial_write_dec(g_next_fill);
        serial_write_str(" queued=");
        serial_write_dec(queued);
        serial_write_str(" energy=");
        serial_write_dec(energy);
        serial_write_str("\n");

        if (sr != 0) {
            // Clear latched status bits
            port_outb((uint16_t)(g_nabm_base + AC97_NABM_POSR), 0x1F);
        }
    }

    // Hardware will play it automatically!
}

void ac97_kick(void) {
    if (!g_ac97_initialized) return;

    // Refill all buffers based on current streams
    for (int i = 0; i < AC97_BD_COUNT; i++) {
        audio_mix_streams(g_audio_buffers[i], AC97_FRAMES_PER_BUFFER);
        g_last_valid = (uint8_t)i;
    }
    g_next_fill = 0;

    // Reset/clear status and restart DMA
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1F);
    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x05);
}

void ac97_set_sample_rate(uint32_t rate_hz) {
    if (!g_ac97_initialized) return;
    if (!g_vra_supported) return;
    if (rate_hz < 8000) rate_hz = 8000;
    if (rate_hz > 48000) rate_hz = 48000;
    ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, (uint16_t)rate_hz);
    serial_write_str("AC97: Set DAC rate=");
    serial_write_dec(rate_hz);
    serial_write_str("\n");
}
