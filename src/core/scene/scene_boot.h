/* The boot intro chain: boot_intro_sequence (1000:B590).
 */

#ifndef HUMANSRE_SCENE_BOOT_H
#define HUMANSRE_SCENE_BOOT_H

#include "osystem.h"

#include <stdint.h>

/**
 * @brief Play the power-on boot intro chain for one frame; 1 = showing.
 *
 * Four stages in order: the Gametek logo (1000:BEE9/B211, GAMETEK.RSC's
 * display list, one group per tick, abortable by key), the IDI card
 * (1000:BD73, its own palette, 0x1a4 hold), the FIRST card (1000:BD13, bank by
 * bank, 0x118 each) and the ITB card + the scene (1000:B63B).
 *
 * The scene plays INTRO\INTRO.RSC over a scrolling INTRO\BAC.BIT with the
 * tree/apple sprites riding the scroll, driven by the 202F:0x843C cue list.
 * Per tick: script_fire, the scroll render (1000:B9F8, 184 rows in 92 pairs),
 * the sprites (1000:BAA5), the display-list group (1000:B955), then present and
 * the frame wait.  The present (1000:BB05) shows pool rows 4-183 as visible
 * rows 0-179 and keeps the bar (rows 182-197, colours 0xE0-0xEF) below it.
 *
 * Plays once; the quit cue (frame 385) ends it and a held key aborts after
 * every wait (1000:B787) and in the transition fade (1000:C0AF) - every
 * stage's c0af/c0b8 wait polls via boot_abort_poll.  The fades run through
 * palette.c's build/step machine (1000:8BFD).
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
