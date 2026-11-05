/* score.h — the interface of src/score.c: check_highscore (1000:41AE)
 * and its score/record screen. */

#ifndef HUMANSRE_SCORE_H
#define HUMANSRE_SCORE_H

#include "osystem.h"

/* check_highscore (1000:41AE), the score/record screen between the level's
 * end page and the post-level newspaper.  START: SCORE.BIT + the score line
 * + the key/ACTION exit and the 0x100-entry fade to 0x48BC (div 0x23). */
/* check_highscore's score terms (1000:41AE): level x members x 10 + the
 * remaining seconds x 10; a lost level is 0.  The placed-object bonuses are
 * the one term the object array (0x3E75) still owes. */
uint32_t score_level_score(void);

/* --- check_highscore's data (1000:41B8..4352) ------------------------- */
uint32_t score_members(void); /* [0x11FC]'s recompute      */
uint16_t score_level_term_for_probe(void); /* [0x1DCA] */
void score_add_level_term(uint16_t v); /* [0x1DCA] += v (1000:C9B9) */
void score_prepare(void); /* terms, best, NEW RECORD, totals */
int32_t score_counters_run(void); /* 1000:475D: one 10-point step */
int32_t score_is_record(void); /* [0x1DD6]                  */
int32_t score_blink_tick(void); /* 1000:467A: 1 = phase flipped */
int32_t score_blink_on(void); /* [0x1DC4]: 1 = the stamp is up */
void score_stamp_draw(uint8_t *page, int32_t on); /* 1000:46FC / 0x46A3 */
void score_column(uint8_t *page, uint32_t value, int32_t x, int32_t y,
                  uint32_t digits);
void score_draw_columns(uint8_t *page); /* 1000:4471                 */
void score_draw_card(uint8_t *page); /* 1000:4403, transparent for 0 */
void score_draw_num3(uint8_t *page, uint32_t ax, uint16_t value, uint32_t bx,
                     uint32_t bp); /* 1000:44BB / 0x450F */
uint16_t score_best_for(uint32_t level); /* DS:0x38A + level*2        */
uint16_t score_best_time(void); /* [0x1E88]                  */
uint32_t score_total_lo(void), score_total_hi(void); /* [0x592]/[0x594]   */
void score_set_total(uint32_t lo, uint32_t hi); /* the save header's halves */
uint32_t score_shown_lo(void), score_shown_hi(void); /* [0x1DD2]/[0x1DD4] */
int32_t score_begin(uint32_t value); /* 1 = the screen is up      */
int32_t score_tick(const InputState *in); /* 1 = still up              */

#endif /* HUMANSRE_SCORE_H */
