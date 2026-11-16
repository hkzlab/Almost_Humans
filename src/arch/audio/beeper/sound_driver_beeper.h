/* BEEPER.DRV's replacement: the sequencer over the PC speaker.
 *
 * Image-relative offsets (the driver is a CD resource):
 *   install   1E95:00DE   cmd 0     1E95:019E   cmd 2     1E95:050C
 *   cmd 5     1E95:02B0   cmd 6     1E95:03F9   cmd 0x0A  1E95:01DB
 *   cmd 0x0C  1E95:022E   cmd 0x0E  1E95:01BE
 *
 * The period table is CS:0x08B2 (84 PIT divisors); the row decoder is
 * 1E95:0660/06BB.  cmd 0x0A is a dead master volume (the apply is a bare RET).
 * The tune image is copied, not aliased.  Only the audio backend and the tests
 * name the module: the core drives it through SoundDriverModule, selected at
 * run time.
 * Deviation: D-254 (bounded table and image reads).
 */
#ifndef HUMANSRE_SOUND_DRIVER_BEEPER_H
#define HUMANSRE_SOUND_DRIVER_BEEPER_H

#include "audio/sound_driver.h"

extern const SoundDriverModule g_sound_driver_beeper;

#endif /* HUMANSRE_SOUND_DRIVER_BEEPER_H */
