/* The MAP flow: menu_loop's phase machine, the fades and the key handling
 * (1000:012A..0x0650).
 */

#ifndef HUMANSRE_MAP_FLOW_H
#define HUMANSRE_MAP_FLOW_H

#include "osystem.h"

#include <stdint.h>

/**
 * @brief Open the MAP screen (menu_loop 1000:012A, after the ending).
 *
 * START: the HIGH.BIT card + the title and the key/ACTION exit.
 *
 * @return 1 when the MAP screen opened, 0 otherwise
 */
uint8_t map_begin(void);

/**
 * @brief One MAP tick; 1 = still up.
 *
 * @param in the input state
 * @return 1 while the MAP screen is up
 */
uint8_t map_tick(const InputState *in);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: the MAP panel [0x8A2] (0 = the entry animation, 1..6).
 *
 * @return the panel
 */
uint16_t map_panel_for_probe(void);

/**
 * @brief Accessor: the frame band's source row [0x8AE].
 *
 * @return the row
 */
uint16_t map_box_bottom_for_probe(void);

/**
 * @brief Accessor: the MAP transition step (0..6, 1000:028C..030A).
 *
 * @return the step
 */
uint8_t map_transition_step(void);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: the MAP fade's start count (palette_build_fade calls).
 *
 * @return the count
 */
uint8_t map_fade_starts(void);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: one MAP fade entry.
 *
 * @param i the index
 * @param first receives the first entry
 * @param entries receives the entry count
 * @param div receives the divisor
 * @param target receives the target value
 * @return 1 when the entry exists, 0 otherwise
 */
int32_t map_fade_at(uint32_t i, uint32_t *first, uint32_t *entries,
                    uint32_t *div, uint32_t *target);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Test/probe seam: 1 after the first draw, 2 after the flip's redraw
 * (1000:0251's two passes).
 *
 * @return the draw pass (1 or 2)
 */
uint8_t map_congrats_draws(void);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief Accessor: the name screen's ring cell (the test seam).
 *
 * @return the cell (0..29)
 */
uint8_t map_name_col_for_probe(void);

/** @brief Accessor: the opening step (the test seam).
 *
 * @return the 1000:021E..0x0285 step
 */
uint8_t map_intro_step_for_probe(void);

/** @brief Accessor: the Enter sequence's phase (the test seam).
 *
 * @return 0 = fading to white, 1 = fading back, 2 = the header is up
 */
uint8_t map_enter_step_for_probe(void);

/** @brief Accessor: the Enter spin's BX counter (the test seam).
 *
 * @return the 0x04B9/0x0515 counter
 */
uint16_t map_enter_bx_for_probe(void);

/** @brief Accessor: the offscreen list page (the test seam).
 *
 * @return the 0x8A4-byte page buffer
 */
uint8_t *map_off_page_for_probe(void);

/** @brief Accessor: the name-entry fire counter (the test seam).
 *
 * @return the [0x5AC8] counter model
 */
uint16_t map_name_fire_wait_for_probe(void);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_MAP_FLOW_H */
