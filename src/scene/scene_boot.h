/* The boot intro chain: boot_intro_sequence (1000:B590).
 */

#ifndef HUMANSRE_SCENE_BOOT_H
#define HUMANSRE_SCENE_BOOT_H

#include "osystem.h"

#include <stdint.h>

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

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief probe hook: the boot state machine's current phase (read-only, for the
 * tests and the diagnostics)
 *
 * @return the boot phase
 */
uint32_t scene_boot_phase_for_probe(void);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_SCENE_BOOT_H */
