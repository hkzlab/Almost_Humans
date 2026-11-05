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
void alpha_draw_wheel(uint8_t *page, uint32_t base);
void alpha_draw_name(uint8_t *page, const char *name);
void map_draw_box_edges(uint8_t *page, uint32_t row); /* 1000:07E6    */
void map_draw_list(uint8_t *page); /* draw_score_block 1000:0651 */
void map_draw_list_header(uint8_t *page); /* 1000:0518's 0x0B5F rows  */
void map_draw_congrats(uint8_t *page); /* 1000:021E..0266            */
uint32_t map_congrats_draws(void); /* 1000:0251's two passes     */
void map_draw_rect(uint8_t *page, uint32_t x, uint32_t y, uint32_t w,
                   uint32_t h); /* 1000:1251 outline          */
uint32_t map_transition_step(void); /* 0..6, 1000:028C..030A      */
uint32_t map_fade_starts(void); /* palette_build_fade calls   */
int32_t map_fade_at(uint32_t i, uint32_t *first, uint32_t *entries,
                    uint32_t *div, uint32_t *target);
void map_draw_name(uint8_t *page); /* 1000:0CBE (ring + typed)   */

/* --- the MAP screen's frame (apl\band.spr, 1000:12D3/0874/07E6) --------- */
extern uint32_t g_map_box_bottom; /* [0x8AE]                    */
extern uint32_t g_map_panel; /* [0x8A2]: 1..6              */

/* --- the band's staging page and four-strip animation (1000:12D3/09DA/
 * 0A72/07B2/0E77/0EBE/0EEC/0F3E) --------------------------------------- */
void map_palette_reset(void); /* 1000:07B2: shadows + two blits  */
uint16_t
map_anim_channel_for_probe(uint32_t ch); /* [0x90]/[0x92]/[0x94]/[0x96] */
void map_anim_tick_for_probe(void); /* one 1000:09DA frame */
uint32_t map_name_col_for_probe(void); /* the name ring cell  */
uint32_t map_name_angle_for_probe(void); /* the ring angle [0x48] */
void map_name_set_angle_for_probe(uint32_t angle); /* the erase check */
uint32_t map_intro_step_for_probe(void); /* the opening step   */
int32_t map_glyph_for_probe(uint32_t i, int32_t *tx, int32_t *ty, int32_t *cx,
                            int32_t *cy); /* the [0x596] entry  */
uint32_t map_name_fire_wait_for_probe(void); /* [0x5AC8] model */
const char *map_name_for_probe(void); /* the typed name      */
int32_t map_stage_ready_for_probe(void); /* the staging page is filled */
uint8_t map_stage_at(uint32_t addr, uint32_t plane); /* 1000:12D3 plane byte */

#endif /* HUMANSRE_MAP_H */
