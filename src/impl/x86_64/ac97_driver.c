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
#define AC97_NAM_RESET              0x00
#define AC97_NAM_MASTER             0x02
#define AC97_NAM_PCM_OUT            0x18
#define AC97_NAM_EXT_AUDIO_ID       0x28
#define AC97_NAM_EXT_AUDIO_CTRL     0x2A
#define AC97_NAM_PCM_FRONT_DAC_RATE 0x2C
#define AC97_NAM_VENDOR_ID1         0x7C
#define AC97_NAM_VENDOR_ID2         0x7E

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
#define AC97_BYTES_PER_FRAME (AC97_CHANNELS * sizeof(int16_t))
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

    // Reset codec and set volumes to max (unmuted, 0 attenuation)
    ac97_mixer_write(AC97_NAM_RESET, 0x0000);
    ac97_mixer_write(AC97_NAM_MASTER, 0x0000);
    ac97_mixer_write(AC97_NAM_PCM_OUT, 0x0000);

    // FIX (Bug 4): Detect VRA and set the DAC sample rate HERE, before any
    // buffer priming or DMA start.  Previously VRA detection happened AFTER
    // port_outb(POCR, 0x05) started the DMA engine, meaning the hardware was
    // already clocking out audio at the wrong (default) rate before ac97_set_
    // sample_rate() was ever called.  With non-48kHz files this caused obvious
    // pitch errors.  Move it first so the rate is locked before playback begins.
    uint16_t ext_id = ac97_mixer_read(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x1) {
        g_vra_supported = true;
        ac97_mixer_write(AC97_NAM_EXT_AUDIO_CTRL, 0x1);
        // Default to 48 kHz; audio_player_play() will call ac97_set_sample_rate
        // with the actual file rate before enabling g_audio_state.playing.
        ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, 48000);
        serial_write_str("AC97: VRA supported, default rate 48000\n");
    } else {
        serial_write_str("AC97: VRA not supported, fixed 48000\n");
    }

    g_bd_list = (ac97_bd_t*)kmalloc_aligned(
        AC97_BD_COUNT * sizeof(ac97_bd_t), 256);

    g_audio_buffer_base = (int16_t*)kmalloc_aligned(
        AC97_BD_COUNT * AC97_BUFFER_BYTES, 4096);

    if (!g_bd_list || !g_audio_buffer_base) {
        serial_write_str("AC97: alloc failed\n");
        return false;
    }

    // samples_per_buffer = frames * channels (interleaved int16_t values)
    uint32_t samples_per_buffer = AC97_FRAMES_PER_BUFFER * AC97_CHANNELS;

    for (int i = 0; i < AC97_BD_COUNT; i++) {
        g_audio_buffers[i] =
            (int16_t*)((uint8_t*)g_audio_buffer_base + (i * AC97_BUFFER_BYTES));

        g_bd_list[i].buffer_addr = (uint32_t)(uintptr_t)g_audio_buffers[i];
        g_bd_list[i].length      = (uint16_t)samples_per_buffer;

        // BUP: on underrun repeat last sample rather than outputting noise.
        // IOC omitted to avoid unnecessary IRQ load; polling via ac97_update.
        g_bd_list[i].flags = AC97_BD_BUP;

        // Zero-fill all hardware buffers so underrun silence is clean.
        // FIX (Bug 5): do NOT call audio_mix_streams here.  At init time
        // g_audio_state.player is NULL, so mix_streams just memsets to zero
        // anyway — but more importantly it would advance the software's
        // g_last_valid pointer through all 32 slots BEFORE any player exists.
        // When audio_player_play() later sets playing=true and ac97_update()
        // starts feeding real audio, the CIV/LVI ring is already out of sync
        // with where the software thinks it is, causing the driver to skip
        // filling buffers the hardware is about to consume → initial silence
        // burst or stutter.  Just memset to zero here.
        memset(g_audio_buffers[i], 0, AC97_BUFFER_BYTES);
    }

    port_outl(g_nabm_base + AC97_NABM_POBDBAR,
              (uint32_t)(uintptr_t)g_bd_list);

    // Clear any stale status bits
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1F);
    // Stop DMA (POCR = 0)
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);

    // FIX (Bug 5 cont.): start g_last_valid at 0 and do NOT advance it here.
    // ac97_update() will fill ahead as soon as audio_player_play() sets
    // g_audio_state.playing = true, keeping g_last_valid correctly chasing CIV.
    g_last_valid = 0;
    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);

    // FIX (Bug 5 cont.): do NOT start DMA here (was port_outb(POCR, 0x05)).
    // DMA is started in ac97_start(), called from audio_player_play() after the
    // sample rate is set and the player state is ready.  Starting DMA at init
    // time caused the hardware to race through silence buffers and wrap the ring
    // before the first real audio arrived, producing a ~1 second silence gap
    // and desynchronising CIV from g_last_valid.
    // port_outb(g_nabm_base + AC97_NABM_POCR, 0x05);  // <-- REMOVED

    g_ac97_initialized = true;
    serial_write_str("AC97: Initialized OK (DMA not started yet)\n");
    return true;
}

// FIX (Bug 5): new function — starts DMA after the player is ready.
// Call this from audio_player_play() after setting g_audio_state.playing = true.
void ac97_start(void) {
    if (!g_ac97_initialized) return;

    // Clear status, then start DMA: RPBM (run/pause bus master) + IOCE
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1F);
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x05);
    serial_write_str("AC97: DMA started\n");
}

// Called from timer interrupt or main loop poll
void ac97_update(void) {
    if (!g_ac97_initialized) return;

    uint8_t civ = port_inb((uint16_t)(g_nabm_base + AC97_NABM_CIV));

    static uint8_t last_civ = 0xFF;
    static uint32_t stall = 0;

    if (civ == last_civ) stall++;
    else { stall = 0; last_civ = civ; }

    // FIX (Bug 10): stall detection threshold was 25 poll ticks.  On a fast
    // system that fires ac97_kick() constantly even during normal brief pauses
    // in audio_update() scheduling.  ac97_kick() refills ALL 32 buffers from
    // scratch via audio_mix_streams(), which advances the ADPCM predictor state
    // by 32 * AUDIO_BUFFER_SIZE samples worth — skipping a large chunk of the
    // file forward — and then resets CIV to 0, causing the "sped up" artifact.
    // Raise the threshold to something that won't false-fire under normal load.
    // 200 ticks at a typical 1ms timer = ~200ms of true silence before kicking.
    if (stall > 200) {
        ac97_kick();
        stall = 0;
        return;
    }

    uint8_t safety_gap = 2;

    for (uint8_t i = 0; i < g_target_ahead; i++) {
        uint8_t next = (g_last_valid + 1) % AC97_BD_COUNT;

        // Don't get too close to the currently playing buffer
        uint8_t dist = (next + AC97_BD_COUNT - civ) % AC97_BD_COUNT;
        if (dist <= safety_gap) break;

        audio_mix_streams(g_audio_buffers[next], AC97_FRAMES_PER_BUFFER);

        g_last_valid = next;
        port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    }

    g_next_fill = (g_last_valid + 1) % AC97_BD_COUNT;
}

// FIX (Bug 10 cont.): ac97_kick() was previously called far too eagerly (stall
// threshold = 25) AND it refilled all 32 buffers from index 0, rewinding the
// hardware ring and advancing the software ADPCM decoder state by a huge amount.
// This produced the "sped up / deep fried" sound on kick.
// Now kick is only called after a genuine ~200ms stall.  When it does fire it
// still resets the ring, but that is acceptable for true underrun recovery.
void ac97_kick(void) {
    if (!g_ac97_initialized) return;

    serial_write_str("AC97: kick (underrun recovery)\n");

    port_outb(g_nabm_base + AC97_NABM_POCR, 0x00);
    port_outb(g_nabm_base + AC97_NABM_POSR, 0x1F);

    for (int i = 0; i < AC97_BD_COUNT; i++) {
        audio_mix_streams(g_audio_buffers[i], AC97_FRAMES_PER_BUFFER);
        g_last_valid = i;
    }

    port_outb(g_nabm_base + AC97_NABM_POLVI, g_last_valid);
    port_outb(g_nabm_base + AC97_NABM_POCR, 0x05);
}

void ac97_set_sample_rate(uint32_t rate_hz) {
    if (!g_ac97_initialized) return;
    if (!g_vra_supported) return;
    if (rate_hz < 8000)  rate_hz = 8000;
    if (rate_hz > 48000) rate_hz = 48000;
    ac97_mixer_write(AC97_NAM_PCM_FRONT_DAC_RATE, (uint16_t)rate_hz);
    serial_write_str("AC97: Set DAC rate=");
    serial_write_dec(rate_hz);
    serial_write_str("\n");
}