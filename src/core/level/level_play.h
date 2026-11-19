/* The level runtime: the start chain, the per-tick spine and the
 * world render.
 */

#ifndef HUMANSRE_LEVEL_PLAY_H
#define HUMANSRE_LEVEL_PLAY_H

#include "osystem.h"
#include <stdint.h>

/**
 * @brief One play frame: update the world, render and pump the effects.
 *
 * @return 1 while the level is running
 */
uint8_t level_tick(void);

/** @brief [0xC], update_world's gate: set by level_fade_in (1000:336C), cleared
 * by the claim at 1000:3B19 and restored at 1000:3CAB.
 *
 * @param on the on/off flag
 */
void level_set_updates(uint8_t on);

/**
 * @brief Accessor: the world-update gate ([0xC]).
 *
 * @return 1 while update_world runs, 0 otherwise
 */
uint8_t level_updates(void);

/** @brief Latch the play loop's [0x5ACE] for the command dispatch (1000:E3F6).
 *
 * 1000:8526's output; the backend already produces the same shape in
 * InputState.joyBits.  Kept out of level_tick's signature so the front-end
 * screens share it.
 *
 * @param in the input state
 */
void level_note_input(const InputState *in);

/**
 * @brief level_fade_in (1000:3334): arm the start chain.
 *
 * Fade out of the pre-level screen, eight update_world frames, the fade in to
 * the working palette, then the bar's ticking gate.  Armed once; the phases
 * advance in level_tick, so level_starting() is true until playable.
 */
void level_begin(void);

/** @brief still inside level_fade_in's chain?  game_main holds the goal page and the
 * play loop until it is done, exactly like the original's foreground block.
 *
 * @return 1 while the fade-in chain runs, 0 otherwise
 */
uint8_t level_starting(void);

/** @brief draw_level_text 0x344C/0x3450: re-render the world page
 * (blit_bitmap_planes + render_objects), erasing the caption before
 * check_highscore's card.
 */
void level_redraw_page(void);

/** @brief draw_level_text 1000:3437: the end page clamps [0x2] to 0x78 before its
 * two page frames; the clamped value persists in the original's view.
 */
void level_end_redraw_page(void);

/**
 * @brief Centre the composed page's world in the field.
 *
 * The world is drawn at the top of the doubled field (rows 0..319 of 400)
 * because the level's own frame puts it there.  The score screen's background
 * is this frozen page with no bar, so it is shifted down by the slack
 * ((field - world) / 2, 40 rows in wide mode) and the padding is cleared -
 * otherwise the greyed level sits at the top of the screen, not centred.
 * A no-op in classic mode and when the field is not taller than the world.
 */
void level_center_page(void);

#endif /* HUMANSRE_LEVEL_PLAY_H */
