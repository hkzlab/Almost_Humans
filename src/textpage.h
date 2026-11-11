/* The in-level text page: the box, its lines and the option markers.
 *
 *   draw_scroll_text     1000:A08E   the measure
 *   draw_map_object      1000:9EA3   the box placement
 *   blit_sprite_planar   1000:A10E   the 9-slice BOX.SPR tiles
 *   blit_text_tile       1000:47D5   the lines
 *   text_page_outline    1000:9FB1
 *   the markers          1000:A203 / A266
 *
 * Deviation: D-258 (the arena is 1,600 B, an oversize glyph is dropped).
 */

#ifndef HUMANSRE_TEXTPAGE_H
#define HUMANSRE_TEXTPAGE_H

#include <stdint.h>

/**
 * @brief Publish the in-level dialog's box tile bank.
 *
 * load_level_assets (1000:3468): apl\box.spr into [0x13B1], 9 tiles of 256 B
 * (0x900 = the file's size exactly); draw_map_object (1000:9EA3) passes the
 * nine 0x100-strided source offsets and textpage_draw is their only reader.
 * NULL (which `level.c` publishes unless the load returned >= 0x900 bytes)
 * means the dialog's lines are drawn on a bare page, with no box.
 *
 * The bank is borrowed, not copied, so it must outlive the publish.
 *
 * @param bank the loaded APL\BOX.SPR bank (9 x 256 B)
 */
void textpage_publish_box(const uint8_t *bank);

/**
 * @brief Release the dialog arena (the [0x1E20] heap block).
 */
void textpage_shutdown(void);

/**
 * @brief Draw a page of text into the page buffer.
 *
 * The block's two header lines (title + blank) come from `text` itself, so
 * `cursorRow` 0 is line 2 of the block. The box tiles come from
 * textpage_publish_box; if none was published the lines are still drawn, on a
 * bare page. Every failure is silent: a NULL page or text, empty text, or more
 * than 20 lines draws nothing at all.
 *
 * @param page      320x200 8bpp page, 320 bytes per scanline (video_page(0))
 * @param text      CR-separated, NUL-terminated block; not modified
 * @param cursorRow 0-based option row for the marker pair; < 0 draws no markers
 * @param cursorCol 0-based cell in that row (3 chars per cell); grid only
 * @param grid      non-zero: markers bracket the number cell; 0: box edges
 */
void textpage_draw(uint8_t *page, const char *text, int32_t cursorRow,
                   uint32_t cursorCol, int32_t grid);

/** @brief One centred line whose glyph pixels are a solid palette index: it
 * writes, replacing whatever is underneath.
 *
 * Only the first CR-terminated line is drawn, and font.c's 44-glyph charset is
 * the whole alphabet: any other byte is skipped, yet font_text_width still
 * charges it 80 px, which shifts the centring.
 *
 * @param page   320x200 8bpp page, 320 bytes per scanline
 * @param line   the text; drawing stops at the first CR
 * @param row    top of the 16-row cell, copied whole (the blit takes all 16
 *               arena rows); the shipped font's ink spans glyph rows 3..14
 * @param colour palette index, stored over the page
 */
void textpage_draw_line(uint8_t *page, const char *line, uint32_t row,
                        uint8_t colour);

/** @brief One centred line OR'd into the page: `mask`'s bits are set where the
 * glyph has ink, everything else is left as it was.
 *
 * blit_text_tile (1000:47D5) as draw_map_object drives it: write mode 2 with
 * the OR ALU (GC3 = 0x1003 at 1000:9F76), `mask` being the Bit Mask (DL, 0x10
 * at 1000:9F90). The result is `page | mask`, never a colour of its own, so
 * over the box's fill tile (whose 16 rows are the ramp EB EA E9 E8 E7 E6 E5
 * E4 E4 E5 E6 E7 E8 E9 EA EB, eight indices that are all (30,0,0) in the bar
 * bank) the glyph becomes the grey ramp F4..FB. That is why the level's goal
 * text is shaded and not flat white.
 *
 * @param page 320x200 8bpp page, 320 bytes per scanline
 * @param line the text; drawing stops at the first CR (see textpage_draw_line
 *             for the charset caveat)
 * @param row  top of the 16-row cell, copied whole (see textpage_draw_line)
 * @param mask the bits OR'd into the page; 0 leaves the page untouched
 */
void textpage_draw_line_mask(uint8_t *page, const char *line, uint32_t row,
                             uint8_t mask);

/**
 * @brief The same masked line centred on an arbitrary x.
 *
 * draw_text's own `AX`: the MAP's list asks for 0x20, 0x82 and 0x102 (its
 * three columns) and its header and congrats lines for 0xA0.
 *
 * @param page    320x200 8bpp page, 320 bytes per scanline
 * @param line    the text; drawing stops at the first CR
 * @param centreX the centring x (0xA0 = the screen centre)
 * @param row     top of the 16-row cell, copied whole (see textpage_draw_line)
 * @param mask    the bits OR'd into the page; 0 leaves the page untouched
 */
void textpage_draw_line_mask_at(uint8_t *page, const char *line,
                                uint32_t centreX, uint32_t row, uint8_t mask);

#endif /* HUMANSRE_TEXTPAGE_H */
