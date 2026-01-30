#ifndef PCAUDIO_H
#define PCAUDIO_H

#include <stdint.h>

// Initialize and play a tone at the given frequency (in Hz)
void play_sound(uint32_t frequency);

// Stop playing any tone (disable speaker)
void nosound(void);

// Play a beep sound at 1kHz for a short duration
void beep(void);

// Optional: a delay function you may need for beep duration (implemented elsewhere)
void timer_wait(uint32_t ticks);

#endif // PCAUDIO_H
