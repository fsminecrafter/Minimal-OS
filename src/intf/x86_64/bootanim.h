#pragma once
/*
 * The boot animation, from early boot until the terminal or mosWM takes over. It is drawn from the timer
 * interrupt (about 30 frames a second) and hides the boot text while it plays. Three styles:
 *
 *   logo       the Minimal-OS logo (Icon.png, src/resources/bootlogo.h) with five dots circling under it
 *   pulse      the logo breathing, a light sweeping across it
 *   hourglass  the logo's hourglass mark turning over again and again
 *
 * `mossettings set bootanimstyle logo|pulse|hourglass` (BootupAnimStyle in 0:/etc/System.conf, read once
 * the disk is mounted: every style starts with the logo fading in), or `bootanim=<style>` on the kernel
 * command line (counts from the first frame). Off with `mossettings set bootupanim false`, the kernel
 * command line `nobootanim`, or `--debug`.
 */
#include <stdbool.h>
#include <stdint.h>
#include "x86_64/multiboot2parse.h"

// Starts it (after the timer, heap and MMIO mapping are up).
void bootanim_start(multiboot2_info_t* mb_info);
// From the PIT interrupt, every tick.
void bootanim_tick(void);
// Draws a frame if one is due (called from the serial log path too).
void bootanim_poll(void);
// No frames while the GPU drivers set up the display.
void bootanim_pause(bool paused);
// Ends it; with `redraw_console` the boot text is drawn again.
void bootanim_stop(bool redraw_console);
// Once the disk is mounted: BootupAnim=false in System.conf ends it.
void bootanim_apply_setting(void);
// True while it owns the screen (the text console only keeps its buffer).
bool bootanim_hides_console(void);
bool bootanim_running(void);
// The desktop is starting: if nothing claims the screen within `ms`, the
// animation ends by itself and the console comes back.
void bootanim_stop_after(uint32_t ms);
// From panic(): stop at once, without waiting or drawing.
void bootanim_abort(void);
