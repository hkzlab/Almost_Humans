/* The level's working palette 202F:0x14D3, the score and end
 * preparations.
 */

#ifndef HUMANSRE_LEVEL_PALETTE_H
#define HUMANSRE_LEVEL_PALETTE_H

#include <stdint.h>

/**
 * @brief the level's working palette 202F:0x14D3.
 *
 * The fade target of the claim, the level's own fades and the rotate hook.
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_palette(void);

/** @brief check_highscore's palette prep (1000:4287/0x428A): seal the working
 * palette's entries 0x80..0xDF to grey (`0x43BC` at `0x1653` = entry 0x80,
 * CX = 0x60), then copy the score card's own 64 entries over 0xC0..0xFF
 * (`0x4403`'s `0xFA00` -> `0x1713`, `0xC0` bytes).
 *
 * @param cardPalette the card's 192 palette bytes, or NULL
 */
void level_palette_score_prepare(const uint8_t *cardPalette);

/** @brief draw_level_text 0x33E2/0x33F2: seal the level palette's 0..0x7F to
 * grey (seal_triples) and copy the shipped gradient into 0x80..0xBF (the
 * 0x14D3 buffer).
 *
 * The 0x33FD fade targets this palette.
 */
void level_end_palette(void);

#endif /* HUMANSRE_LEVEL_PALETTE_H */
