/* The silent driver: logs every INT 60h command, no synth underneath.
 *
 * Port-only, kept for a port that links it; no current target does, so
 * nothing includes this header yet.  Only an audio backend would name the
 * module.
 */
#ifndef HUMANSRE_SOUND_DRIVER_NULL_H
#define HUMANSRE_SOUND_DRIVER_NULL_H

#include "audio/sound_driver.h"

extern const SoundDriverModule g_sound_driver_null;

#endif /* HUMANSRE_SOUND_DRIVER_NULL_H */
