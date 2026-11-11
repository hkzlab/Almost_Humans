/* The AdLib sound-driver module's one exported symbol.
 *
 * Only the audio backend and the tests name the module: the core drives it
 * through SoundDriverModule, selected at run time.  Deviation: D-249.
 */
#ifndef HUMANSRE_SOUND_DRIVER_ADLIB_H
#define HUMANSRE_SOUND_DRIVER_ADLIB_H

#include "audio/sound_driver.h"

extern const SoundDriverModule g_sound_driver_adlib;

#endif /* HUMANSRE_SOUND_DRIVER_ADLIB_H */
