#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "x86_64/audio_hw.h"

// Intel High Definition Audio (HDA / "Azalia") controller driver.
// Works with any HDA controller (PCI class 04, subclass 03) and codecs
// that follow the HDA spec: QEMU's intel-hda/ich9-intel-hda with
// hda-output or hda-duplex, and the Realtek/Conexant/IDT codecs on
// real machines.
//
// Like ac97_driver.h, the only thing the rest of the kernel needs is
// the audio_hw_driver_t vtable for audio_manager_register_driver().
const audio_hw_driver_t* hda_get_driver(void);
