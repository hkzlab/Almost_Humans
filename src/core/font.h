/* Text: TEXT.SPR's 44 glyphs, draw_text and blit_glyph.
 *
 *   draw_text     1000:4989   measure, centre, blit
 *   blit_glyph    1000:49FC   the nibble scatter into the arena
 *
 * The shipped charset and advance tables are at 202F:0x1E28/0x1E55.
 * Deviations: D-122 (negative x clamped), D-258 (bounded arena write).
 */

#ifndef HUMANSRE_FONT_H
#define HUMANSRE_FONT_H

#include "osystem.h"

/**
 * @brief Reset the font module's per-boot state (the arena warning latch).
 *
 * @return 0 on success
 */
int32_t font_init(void);

/**
 * @brief Publish the TEXT.SPR glyph bank.
 *
 * game_loop's publish ([0x1E1C] := the loaded apl\text.spr).
 * NULL clears it (drawText then draws nothing) - the headless no-data case.
 *
 * @param textSpr the TEXT.SPR glyph bank
 */
void font_publish(const uint8_t *textSpr);

/** @brief draw_text (1000:4989): centre `s` on `centreX` and blit it at arena
 * row `row`.  Unknown characters advance by the terminator slot but draw
 * nothing (the two search loops' no-match paths).
 *
 * The arena is 200 rows x 80 bytes, 1 bpp, 4 px per byte, bit 0 leftmost;
 * each 2-bpp glyph pixel doubles horizontally into two 1-bpp pixels.
 *
 * @param arena the text arena
 * @param s the text
 * @param centreX the centring x
 * @param row the row
 */
void font_draw_text(uint8_t *arena, const char *s, uint32_t centreX,
                    uint32_t row);

/** @brief draw_text into a bounded arena: a glyph whose 16-row tile would pass
 * `capacity` is dropped, and the first drop is reported through the OSystem's
 * `warn`.  The original writes through ES unbounded (deviation D-258);
 * `textpage.c` passes its arena's size here.
 *
 * @param arena the text arena
 * @param capacity the arena size in bytes
 * @param s the text
 * @param centreX the centring x
 * @param row the row
 */
void font_draw_text_bounded(uint8_t *arena, uint32_t capacity, const char *s,
                            uint32_t centreX, uint32_t row);

/** @brief draw_text's own measure: the sum of the _advance table.
 *
 * The same walk draw_text does before centring; stops at CR/NUL.
 *
 * @param s the text
 * @return the text width in pixels
 */
uint32_t font_text_width(const char *s);

#endif /* HUMANSRE_FONT_H */
