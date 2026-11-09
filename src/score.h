/* score.h - the interface of src/score.c: check_highscore (1000:41AE)
 * and its score/record screen. */

#ifndef HUMANSRE_SCORE_H
#define HUMANSRE_SCORE_H

#include "osystem.h"

/* check_highscore (1000:41AE), the score/record screen between the level's
 * end page and the post-level newspaper.  START: SCORE.BIT + the score line
 * + the key/ACTION exit and the 0x100-entry fade to 0x48BC (div 0x23). */

/** @brief check_highscore's score terms (1000:41AE).  [0x1F41] = the level number
 * (1-based, the same value draw_level_text prints), [0x11FC] = the members.
 *
 * check_highscore's score terms (1000:41AE): level x members x 10 + the
 * remaining seconds x 10; a lost level is 0.
 * The placed-object bonuses are the one term the object array (0x3E75) still
 * owes.
 *
 * @return the level's score
 */
uint32_t score_level_score(void);

/* --- check_highscore's data (1000:41B8..4352) ------------------------- */

/**
 * @brief 1000:41B8..41E6: the member count the score uses ([0x11FC]'s recompute).
 *
 * @return the member count
 */
uint16_t score_members(void);

/** @brief Accessor: the level's score term [0x1DCA] (the test seam for the
 * cross-level leftover).
 *
 * @return the level term
 */
uint16_t score_level_term_for_probe(void);

/**
 * @brief 1000:C9B9: handler 4 adds 0x7D0 to the level's score term [0x1DCA].
 *
 * @param v the level score
 */
void score_add_level_term(uint16_t v);

/**
 * @brief Build the score screen's card (terms, best, NEW RECORD, totals).
 */
void score_prepare(void);

/**
 * @brief 1000:475D tick_score_counters: one 10-point step of the displayed total.
 *
 * @return 1 while the counters still step, 0 when done
 */
uint8_t score_counters_run(void);

/**
 * @brief Accessor: the score set a record ([0x1DD6]).
 *
 * @return The score set a record.
 */
uint8_t score_is_record(void);

/**
 * @brief Advance the score screen's blink (1000:467A: 1 = phase flipped).
 *
 * @return the new blink phase
 */
uint8_t score_blink_tick(void);

/**
 * @brief the stamp's current phase ([0x1DC4]): 1 = drawn.
 *
 * @return the current blink phase (1 = stamp drawn)
 */
uint8_t score_blink_on(void);

/**
 * @brief Draw the record stamp (1000:46FC / 0x46A3).
 *
 * @param page the page buffer
 * @param on the on/off flag
 */
void score_stamp_draw(uint8_t *page, uint8_t on);

/**
 * @brief Draw one score column.
 *
 * @param page the page buffer
 * @param value the value
 * @param x the x coordinate
 * @param y the y coordinate
 * @param digits the digit count
 */
void score_column(uint8_t *page, uint32_t value, int32_t x, int32_t y,
                  uint32_t digits);

/** @brief 1000:4471 draw_score_columns: the level term, the time term and the running
 * total.  Column x is the pixel origin; each digit is 16 px wide.
 *
 * @param page the page buffer
 */
void score_draw_columns(uint8_t *page);

/** @brief 1000:4403 reblit_framebuffer_88's card pass: SCORE.BIT is a chunky 320x200
 * image (one byte per pixel); the four plane passes read `src[4j+p]` into
 * plane p at address j, i.e. pixel 4j+p = src[4j+p] - a plain 1:1 copy. A
 * non-zero byte is drawn as 0xC0 + v, a zero byte is LEFT ALONE (the page
 * keeps the level caption). The file's first 64 palette entries are the
 * card's own DAC 0xC0..0xFF bank.
 *
 * @param page the page buffer
 */
void score_draw_card(uint8_t *page);

/**
 * @brief Draw a three-digit number (1000:44BB / 0x450F).
 *
 * @param page the page buffer
 * @param ax the x accumulator
 * @param value the value
 * @param bx the x value
 * @param bp the page base
 */
void score_draw_num3(uint8_t *page, uint32_t ax, uint16_t value, uint32_t bx,
                     uint32_t bp);

/**
 * @brief Accessor: the best score for one level (DS:0x38A + level*2).
 *
 * @param level the level number
 * @return The best score for one level.
 */
uint16_t score_best_for(uint32_t level);

/**
 * @brief Accessor: the best time ([0x1E88]).
 *
 * @return The best time.
 */
uint16_t score_best_time(void);

/**
 * @brief Accessor: the total score's low word ([0x592]).
 *
 * @return The total score's low word.
 */
uint16_t score_total_lo(void);

/**
 * @brief Accessor: the total score's high word ([0x594]).
 *
 * @return The total score's high word.
 */
uint16_t score_total_hi(void);

/** @brief 1000:42CE..4352: the best-time comparison, the NEW RECORD flag and the
 * running total (the save header's halves).
 *
 * @param lo the low word
 * @param hi the high word
 */
void score_set_total(uint32_t lo, uint32_t hi);

/**
 * @brief Accessor: the shown score's low word ([0x1DD2]).
 *
 * @return The shown score's low word.
 */
uint32_t score_shown_lo(void);

/**
 * @brief Accessor: the shown score's high word ([0x1DD4]).
 *
 * @return The shown score's high word.
 */
uint32_t score_shown_hi(void);

/**
 * @brief Open the score/record screen for one level's score.
 *
 * @param value the value
 * @return 1 when the score screen opened, 0 otherwise
 */
uint8_t score_begin(uint32_t value);

/**
 * @brief One score-screen tick; 1 = still up.
 *
 * @param in the input state
 * @return 1 while the score screen is up
 */
uint8_t score_tick(const InputState *in);

#endif /* HUMANSRE_SCORE_H */
