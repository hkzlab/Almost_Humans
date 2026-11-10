/* The .ANI/.RSC interpreter: the display list, the frame-keyed cues and the
 * scene chain.
 *
 *   cd_file_op              1000:AFF8   one level intro, per tick
 *   blit_background_data    1000:B2EB   the AL expander
 *   blit_display_list       1000:B955
 *   run_screen_transition   1000:B82E
 *   boot_intro_sequence     1000:B590   the boot intro
 *
 * Deviation: D-248 (the evolution scene waits 4 ticks, not 2).
 */

#ifndef HUMANSRE_SCENE_H
#define HUMANSRE_SCENE_H

#include "osystem.h"

/**
 * @brief Bind the scene module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t scene_init(const OSystem *osys);

/**
 * @brief Play one level's .ANI newspaper for a frame (the cd_file_op body).
 *
 * @param introIndex the level header's +0x01 byte: the intro-movie index (0..9;
 *                  0xFF = the level has no cutscene)
 * @return 1 while the newspaper is still showing
 */
uint8_t scene_play_level_intro(uint16_t introIndex);

/**
 * @brief Test seam: skip the intro's per-frame waits ([0x627C]).
 *
 * boot_intro_sequence's fast mode: the scene player fades 4 palette entries
 * div 2 instead of 0x100 div 0x19 and waits 1 tick per object instead of 6.
 *
 * @param on the on/off flag
 */
void scene_set_fast_mode(uint8_t on);

/**
 * @brief Force the next scene_play_level_intro call to reload its .ANI.
 *
 * cd_file_op always reloads, so two consecutive levels with the same intro
 * index still get a new scene; the per-tick player would otherwise keep the
 * finished one finished.
 */
void scene_reset_intro(void);

/**
 * @brief Play the power-on boot intro chain for one frame; 1 = showing.
 *
 * The boot intro (boot_intro_sequence, 1000:B590): the full four-stage chain -
 * the Gametek logo (GAMETEK.RSC's display list), the IDI and FIRST cards, then
 * the ITB card + INTRO.RSC's display list over the scrolled bac.bit background
 * - driven by the 202F:0x843C cue list.
 * Per-tick like the level intro; plays once (a held key aborts the whole chain
 * - every stage's c0af/c0b8 wait polls via boot_abort_poll).
 *
 * @return 1 while the boot intro is still showing
 */
uint8_t scene_play_boot_intro(void);

/**
 * @brief Latch this tick's input for the scene waits.
 *
 * The main loop's per-tick input state; the boot's abort polls (c0c6's
 * [0x48A3] / +0x5ACE bit-7 equivalents) read this cached copy.
 *
 * @param in the input state
 */
void scene_note_input(const InputState *in);

/** @brief probe hook: the boot state machine's current phase (read-only, for the
 * tests and the diagnostics)
 *
 * @return the boot phase
 */
uint32_t scene_boot_phase_for_probe(void);

#endif /* HUMANSRE_SCENE_H */
