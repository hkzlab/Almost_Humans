/* font.h — the interface of src/font.c: glyphs/packed ASCII (subsystem 12, §9) */

#ifndef HUMANSRE_FONT_H
#define HUMANSRE_FONT_H

#include "osystem.h"

int32_t font_init(const OSystem *osys);
/* game_loop's publish ([0x1E1C] := the loaded apl\text.spr).  NULL clears
 * it (drawText then draws nothing) — the headless no-data case. */
void font_publish(const uint8_t *textSpr);
/* draw_text (1000:4989): centre `s` on `centreX` (the advance table,
 * re_formats.md §`TEXT.SPR`) and blit its glyphs into `arena` — the
 * caller's [0x1E20] text arena (200 rows x 80 B, 1 bpp, 4 px per byte,
 * bit 0 = leftmost; each 2-bpp glyph pixel becomes two 1-bpp pixels, so
 * the glyph doubles horizontally) — at `row`. */
void font_draw_text(uint8_t *arena, const char *s, uint32_t centreX,
                    uint32_t row);
/* draw_text's own measure: the advance-table sum (stops at CR/NUL). */
uint32_t font_text_width(const char *s);

#endif /* HUMANSRE_FONT_H */
