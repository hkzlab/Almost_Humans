/* map.h — the interface of src/map.c: the MAP screen, its letter wheel
 * and the draw helpers. */

#ifndef HUMANSRE_MAP_H
#define HUMANSRE_MAP_H

#include "osystem.h"

/* menu_loop (1000:012A), the MAP/high-score screen after the ending.
 * START: the HIGH.BIT card + the title, key/ACTION exit; the full
 * map/cursor/score tables are the next pass. */
int32_t map_begin(void); /* 1 = the screen is up      */
int32_t map_tick(const InputState *in); /* 1 = still up              */

/* --- the MAP screen's letter wheel (apl\alpha.spr, 1000:0705/0F93/0CBE) --- */
void alpha_expand(const uint8_t *src, uint8_t *dst);
void alpha_cell(uint32_t cell, uint32_t angle, uint32_t *src, uint32_t *dst);
void alpha_drawWheel(uint8_t *page, uint32_t base);
void alpha_drawName(uint8_t *page, const char *name);
void map_bandFill(uint8_t *page); /* 1000:12D3                  */
void map_drawBoxEdges(uint8_t *page, uint32_t row); /* 1000:07E6    */
void map_drawList(uint8_t *page); /* draw_score_block 1000:0651 */
void map_drawCongrats(uint8_t *page); /* 1000:021E..0266            */
uint32_t map_congratsDraws(void); /* 1000:0251's two passes     */
void map_drawRect(uint8_t *page, uint32_t x, uint32_t y, uint32_t w,
                  uint32_t h); /* 1000:1251 outline          */
uint32_t map_transitionStep(void); /* 0..6, 1000:028C..030A      */
uint32_t map_fadeStarts(void); /* palette_build_fade calls   */
int32_t map_fadeAt(uint32_t i, uint32_t *first, uint32_t *entries,
                   uint32_t *div, uint32_t *target);
void map_drawName(uint8_t *page); /* 1000:0CBE (ring + typed)   */

/* --- the MAP screen's frame (apl\band.spr, 1000:12D3/0874/07E6) --------- */
extern uint32_t map_boxBottom; /* [0x8AE]                    */
extern uint32_t map_panel; /* [0x8A2]: 1..6              */

#endif /* HUMANSRE_MAP_H */
