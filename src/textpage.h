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

/**
 * @brief Publish the text box's tile bank.
 *
 * @param box the box address
 */
void textpage_publish_box(const uint8_t *box);

/**
 * @brief Draw a page of text into the page buffer.
 *
 * `text` = the block (CR-separated lines, NUL-terminated); cursorRow >= 0
 * draws the marker pair on that option row (grid = the cell pair).
 *
 * @param page the page buffer
 * @param text the text
 * @param cursorRow the cursor row
 * @param cursorCol the cursor column
 * @param grid the grid
 */
void textpage_draw(uint8_t *page, const char *text, int32_t cursorRow,
                   uint32_t cursorCol, int32_t grid);

/** @brief One centred line on the page at `row` in a solid `colour` (not the
 * blit_text_tile path — used where there is no box underneath).
 *
 * one centred line in a solid colour (no box underneath) — the disk prompt,
 * the score screen and the MAP's list use this.
 *
 * @param page the page buffer
 * @param line the text line
 * @param row the row
 * @param colour the colour
 */
void textpage_draw_line(uint8_t *page, const char *line, uint32_t row,
                        uint8_t colour);

/**
 * @brief The same line centred on an arbitrary x (draw_text's AX).
 *
 * @param page the page buffer
 * @param line the text line
 * @param centreX the centring x
 * @param row the row
 * @param colour the colour
 */
void textpage_draw_line_at(uint8_t *page, const char *line, uint32_t centreX,
                           uint32_t row, uint8_t colour);

/** @brief One centred line blitted the original way: `mask` OR'd into the page.
 * The box's fill tile is a vertical ramp EB..E4..EB (all the same red in the
 * palette) so the text becomes the grey ramp FB..F4..FB; that is why the
 * level's goal text is shaded and not flat white.
 *
 * blit_text_tile (1000:47D5) proper: `mask` OR'd into the page (the OR ALU at
 * 1000:9F76 + write mode 2).
 * The box text and the cutscene headline use it, so their shading is the box
 * fill tile's own EB..E4..EB ramp.
 *
 * @param page the page buffer
 * @param line the text line
 * @param row the row
 * @param mask the mask
 */
void textpage_draw_line_mask(uint8_t *page, const char *line, uint32_t row,
                             uint8_t mask);

/**
 * @brief The same masked line centred on an arbitrary x.
 *
 * @param page the page buffer
 * @param line the text line
 * @param centreX the centring x
 * @param row the row
 * @param mask the mask
 */
void textpage_draw_line_mask_at(uint8_t *page, const char *line,
                                uint32_t centreX, uint32_t row, uint8_t mask);

#endif /* HUMANSRE_TEXTPAGE_H */
