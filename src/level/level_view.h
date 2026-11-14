/* The display window: the camera follower, its offset walk and
 * menu_flag_tick's goal pan.
 */

#ifndef HUMANSRE_LEVEL_VIEW_H
#define HUMANSRE_LEVEL_VIEW_H

#include <stdint.h>

/** @brief [0x0]/[0x2]: the display window into the 512x320 level page, in pixels and
 * rows - `set_display_ptr` (1000:882D) starts the CRTC at
 * `pagebase + 0x58*[0x2] + [0x0]/4`, and the object pass subtracts the same
 * pair (`draw_object_scaled_x1` 1000:66EC clips `x - [0x0]` against
 * [-0x10,0x140]).  The two stores are level_reset_view's (`1000:319D`), and in
 * play `level_follow_view` below is their only writer.
 *
 * 1000:3DC0: the view's world origin (the CRTC start's source).
 *
 * @param x the x coordinate
 * @param y the y coordinate
 */
void level_set_view(uint16_t x, uint16_t y);

/** @brief 1000:5CB6 - the first call of `update_world` (`1000:80AD`), guarded there by
 * `[0xC] != 0`: the camera follows the **acting record** (`get_object_ptr`,
 * `1000:79F4`, walks the 29-word table at `0x3E24` with `[0x3E22]`, i.e.
 * `object_core_slot(object_sequence_action_entity())`), or the record `[0x2D8C]` names while
 * menu_flag_tick's goal pan holds it (`1000:5CB9..5CC0`).
 *
 *   target = record + the offset pair at `[0x2D90]` - (0x88, 0x44)
 *   clamped to the pan range: 0xC0 = 512-320 px, 0xA8 = 320-152 rows (the
 *   play area; the bar owns the last 40 rows), then each axis moves at most
 *   `[0x2DA6]` = 4 px per frame (0x5D09 / 0x5D21), stored at 0x5D2F/0x5D33.
 *
 * The offset pair is **{0,0} at rest**: `[0x2D90]` = 0x2D92 (the first pair of
 * the 0x2D92..0x2DA2 table) and `[0x2D8E]` = 0xFFFF, whose bit 15 idles the
 * 4-entry +-1 px walk at 0x5D37.  Both enablers (`0xDAC7` the entity path and
 * `0xF8CE` the fall machine) are ported; _level_offset_walk advances the pair.
 * `update_view` (1000:80F1) then scrolls the page by the difference; the flat
 * compose reads the window directly, which is the same visible result.
 */
void level_follow_view(void);

/**
 * @brief [0x2D8E]: the follower's +-1 px offset walk enabler (1000:DAC7 /
 * 0xF8CE).
 *
 * @param v the offset word
 */
void level_set_offset_word(uint16_t v);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: the record's offset word.
 *
 * @return the word
 */
uint16_t level_offset_word(void);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Test seam: 1000:5D37's walk pointer ([0x2D90]) back to the table's
 * first pair.
 */
void level_reset_offset_walk(void);
#endif /* HUMANSRE_TEST_PROBES */

/**
 * @brief Accessor: the camera's x.
 *
 * @return the x
 */
uint16_t level_view_x(void);

/**
 * @brief Accessor: the camera's y.
 *
 * @return the y
 */
uint16_t level_view_y(void);

/**
 * @brief Accessor: menu_flag_tick's goal pan (1000:3540) is running.
 *
 * The original's pan is a blocking loop, so input_dispatch_loop (1000:349E)
 * does not reach pause_toggle, scan_id_list or the [0x1979] dispatch while it
 * runs; the port's play loop skips them on this flag.
 *
 * @return 1 while the pan runs, 0 otherwise
 */
uint8_t level_goal_pan_running(void);

#endif /* HUMANSRE_LEVEL_VIEW_H */
