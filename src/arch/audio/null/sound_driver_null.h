/* The silent sound-driver module's one exported symbol.
 *
 * Kept for a port that links it; no current target does, so nothing
 * includes this header yet.  Only an audio backend would name it.
 */
#ifndef HUMANSRE_SOUND_DRIVER_NULL_H
#define HUMANSRE_SOUND_DRIVER_NULL_H

#include "audio/sound_driver.h"

extern const SoundDriverModule g_sound_driver_null;

#endif /* HUMANSRE_SOUND_DRIVER_NULL_H */
