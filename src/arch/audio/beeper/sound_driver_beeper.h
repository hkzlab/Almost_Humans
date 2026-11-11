/* The Beeper sound-driver module's one exported symbol.
 *
 * Only the audio backend and the tests name the module: the core drives it
 * through SoundDriverModule, selected at run time.  Deviation: D-254.
 */
#ifndef HUMANSRE_SOUND_DRIVER_BEEPER_H
#define HUMANSRE_SOUND_DRIVER_BEEPER_H

#include "audio/sound_driver.h"

extern const SoundDriverModule g_sound_driver_beeper;

#endif /* HUMANSRE_SOUND_DRIVER_BEEPER_H */
