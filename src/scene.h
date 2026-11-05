/* scene.h — the interface of src/scene.c: .ANI/.RSC interpreter (subsystem 11, §8.5-8.7a) */

#ifndef HUMANSRE_SCENE_H
#define HUMANSRE_SCENE_H

#include "osystem.h"

int32_t scene_init(const OSystem *osys);
/* introIndex = the level header's +0x01 byte: the intro-movie index
   (0..9; 0xFF = the level has no cutscene). */
int32_t scene_play_level_intro(uint32_t introIndex); /* the cd_file_op body   */
/* [0x627C]: boot_intro_sequence's fast mode (its only writer).  The shared
 * scene player then fades 4 palette entries div 2 instead of 0x100 div 0x19
 * and waits 1 tick per object instead of 6.  The level's .ANI runs clear. */
void scene_set_fast_mode(int32_t on);
/* Force the next scene_play_level_intro call to load its .ANI even when the
 * index equals the last one (cd_file_op always reloads; the per-tick player
 * keeps a finished intro finished otherwise). */
void scene_reset_intro(void);
/* The boot intro (boot_intro_sequence, 1000:B590): the full four-stage
 * chain — the Gametek logo (GAMETEK.RSC's display list), the IDI and FIRST
 * cards, then the ITB card + INTRO.RSC's display list over the scrolled
 * bac.bit background — driven by the 202F:0x843C cue list.  Per-tick like
 * the level intro; plays once (a held key aborts the whole chain — every
 * stage's c0af/c0b8 wait polls via boot_abort_poll). */
int32_t scene_play_boot_intro(void);
/* The main loop's per-tick input state; the boot's abort polls (c0c6's
 * [0x48A3] / +0x5ACE bit-7 equivalents) read this cached copy. */
void scene_note_input(const InputState *in);
uint32_t scene_boot_phase_for_probe(void); /* probe: the boot machine's phase */

#endif /* HUMANSRE_SCENE_H */
