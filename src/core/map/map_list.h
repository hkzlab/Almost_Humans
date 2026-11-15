/* The MAP screen's score pages: the top-ten list, its header frames and the
 * qualifying page (1000:021E/0518/0577/1251).
 */

#ifndef HUMANSRE_MAP_LIST_H
#define HUMANSRE_MAP_LIST_H

#include <stdint.h>

/** @brief draw_score_block (1000:0651): the rank is centred at x=0x20, the name at
 * x=0x82, the 32-bit score at x=0x102, row pitch [0x36]=0x14 from base
 * [0x38]=0.
 *
 * `1000:0577` clears the **offscreen** page and `0x0595` draws the rows into
 * it; the displayed page keeps high.bit + the header.  The frame bands
 * (`0x07E6`) copy from this page into the display.
 *
 * @param page the offscreen page buffer
 */
void map_draw_list(uint8_t *page);

/** @brief 1000:0518's header frames: the five `0x0B5F` calls (15 outlines at y =
 * 0x2D + 0x14*i, x = 0x12/0x32/0xD6) and the title at 0xA0/0x14. The page is
 * then cleared at 0x0577, so these only live on the transition.
 *
 * @param page the page buffer
 */
void map_draw_list_header(uint8_t *page);

/**
 * @brief 1000:021E..1000:0266: the seven-line congratulation page.
 *
 * @param page the page buffer
 */
void map_draw_congrats(uint8_t *page);

/** @brief 1000:1251: a rectangle outline in the flat page.  The top edge ORs 0x40,
 * the left and bottom 0x50, the right 0x40 (1000:1263..1298).
 *
 * @param page the page buffer
 * @param x the x coordinate
 * @param y the y coordinate
 * @param w the width
 * @param h the height
 */
void map_draw_rect(uint8_t *page, uint32_t x, uint32_t y, uint32_t w,
                   uint32_t h);

#endif /* HUMANSRE_MAP_LIST_H */
