/* audio.h — the interface of src/audio.c: tune/sfx state machine (subsystem 7) */

#ifndef HUMANSRE_AUDIO_H
#define HUMANSRE_AUDIO_H

#include "osystem.h"

int32_t audio_init(const OSystem *osys);
/* The frame web's music gate (1000:A5E8): `off` = the MUSIC board's counter
 * [0x5E5B] non-zero -> music_stop, zero -> music_resume.  One call per room
 * tick.  The M5 stub's stop/start is the whole observable (M6 adds the tune
 * handle). */
void audio_musicGate(uint32_t off);

#endif /* HUMANSRE_AUDIO_H */
