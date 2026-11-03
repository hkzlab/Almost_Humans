/* textpage.h — the interface of src/textpage.c: the in-level text page (draw_map_object 9EA3)
 *
 * The dialog box + its lines + the option markers, ported from
 * draw_scroll_text / draw_map_object / blit_sprite_planar / blit_text_tile
 * (re_map.md §"The in-level pause menu").  The box tiles are APL\BOX.SPR
 * (9 x 16x16, loaded with the level's assets).
 */

#ifndef HUMANSRE_TEXTPAGE_H
#define HUMANSRE_TEXTPAGE_H

#include <stdint.h>
void textpage_publishBox(const uint8_t *box);
/* `text` = the block (CR-separated lines, NUL-terminated); cursorRow >= 0
 * draws the marker pair on that option row (grid = the cell pair). */
void textpage_draw(uint8_t *page, const char *text, int32_t cursorRow,
                   uint32_t cursorCol, int32_t grid);
/* one centred line in a solid colour (no box underneath) — the disk prompt,
 * the score screen and the MAP's list use this. */
void textpage_drawLine(uint8_t *page, const char *line, uint32_t row,
                       uint8_t colour);
/* draw_text's AX: the line centred on an arbitrary x. */
void textpage_drawLineAt(uint8_t *page, const char *line, uint32_t centreX,
                         uint32_t row, uint8_t colour);
/* blit_text_tile (1000:47D5) proper: `mask` OR'd into the page (the OR ALU at
 * 1000:9F76 + write mode 2).  The box text and the cutscene headline use it,
 * so their shading is the box fill tile's own EB..E4..EB ramp. */
void textpage_drawLineMask(uint8_t *page, const char *line, uint32_t row,
                           uint8_t mask);
void textpage_drawLineMaskAt(uint8_t *page, const char *line, uint32_t centreX,
                             uint32_t row, uint8_t mask);

#endif /* HUMANSRE_TEXTPAGE_H */
