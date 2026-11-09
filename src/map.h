/* map.h - the interface of src/map.c: the MAP screen, its letter wheel
 * and the draw helpers. */

#ifndef HUMANSRE_MAP_H
#define HUMANSRE_MAP_H

#include "osystem.h"

/**
 * @brief Open the MAP screen.
 *
 * menu_loop (1000:012A), the MAP/high-score screen after the ending.
 * START: the HIGH.BIT card + the title, key/ACTION exit; the full
 * map/cursor/score tables are the next pass.
 *
 * @return 1 when the MAP screen opened, 0 otherwise
 */
uint8_t map_begin(void);

/**
 * @brief One MAP tick; 1 = still up.
 *
 * @param in the input state
 * @return 1 while the MAP screen is up
 */
uint8_t map_tick(const InputState *in);

/* --- the MAP screen's letter wheel (apl\alpha.spr, 1000:0705/0F93/0CBE) --- */

/** @brief 1000:0705 expand_nibbles_4plane: the packed 4bpp sheet into four planes of
 * 0x4000 bytes (a literal port of the bit twiddling).
 *
 * @param src the source buffer
 * @param dst the destination buffer
 */
void alpha_expand(const uint8_t *src, uint8_t *dst);

/** @brief 1000:0F93: the ring angle -> (destination byte, source offset).  The x is
 * `157 + 132*cos` px, the y `79 + 124*sin` quarter-rows; the page stride is 80
 * and the `+0xF0` adds three rows.
 *
 * @param cell the map cell
 * @param angle the rotation angle
 * @param src the source buffer
 * @param dst the destination buffer
 */
void alpha_cell(uint32_t cell, uint32_t angle, uint16_t *src, uint16_t *dst);

/**
 * @brief Draw the letter wheel.
 *
 * @param page the page buffer
 * @param base the base offset
 */
void alpha_draw_wheel(uint8_t *page, uint32_t base);

/** @brief 1000:0CBE init_display_lists' name pass, stateless: the typed
 * characters at their final row-0x45 slots.
 *
 * @param page the page buffer
 * @param name the name
 */
void alpha_draw_name(uint8_t *page, const char *name);

/**
 * @brief 1000:07E6: the five frame bands from band row `[0x8AE]`.
 *
 * Each band reads `[0x8AE]*80 + 6` from the offscreen page (`0x0577` cleared
 * it and `0x0595` filled it with the rows; `0x0874` sets `DS = [0x8A4]`) and
 * advances 0x640 per box, writing the masked 4-pixel groups into the display
 * at `A000:[0x5AC4]`.
 *
 * @param page the page buffer
 * @param row the source row ([0x8AE])
 */
void map_draw_box_edges(uint8_t *page, uint32_t row);

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

/**
 * @brief Test/probe seam: 1 after the first draw, 2 after the flip's redraw
 * (1000:0251's two passes).
 *
 * @return the draw pass (1 or 2)
 */
uint8_t map_congrats_draws(void);

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

/**
 * @brief Accessor: the MAP transition step (0..6, 1000:028C..030A).
 *
 * @return The MAP transition step.
 */
uint8_t map_transition_step(void);

/**
 * @brief Accessor: the MAP fade's start count (palette_build_fade calls).
 *
 * @return The MAP fade's start count.
 */
uint8_t map_fade_starts(void);

/**
 * @brief Accessor: one MAP fade entry.
 *
 * @param i the index
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param target the target value
 * @return One MAP fade entry.
 */
int32_t map_fade_at(uint32_t i, uint32_t *first, uint32_t *entries,
                    uint32_t *div, uint32_t *target);

/**
 * @brief 1000:0CBE: clear the previous frame's cells, draw the 30-cell ring at
 * the current angle and the typed name at its display-list positions.
 *
 * @param page the page buffer
 */
void map_draw_name(uint8_t *page);

/* --- the MAP screen's frame (the offscreen row page, 1000:0577/0595/0874/07E6) */
extern uint16_t g_map_box_bottom; /* [0x8AE]                    */
extern uint16_t g_map_panel; /* [0x8A2]: 1..6              */

/* --- the band's staging page and four-strip animation (1000:12D3/09DA/
 * 0A72/07B2/0E77/0EBE/0EEC/0F3E) --------------------------------------- */

/** @brief 1000:07B2 palette_reset: seed the shadows and animate twice (the
 * original's two planar page copies are the flat page itself in the port).
 */
void map_palette_reset(void);

/** @brief Accessor: one animation channel word (the test seam), the four
 * words at [0x90]/[0x92]/[0x94]/[0x96].
 *
 * @param ch the channel (0..3)
 * @return the word, or 0 out of range
 */
uint16_t map_anim_channel_for_probe(uint32_t ch);

/** @brief Advance the band animation one frame (the test seam for 1000:09DA).
 */
void map_anim_tick_for_probe(void);

/** @brief Accessor: the name screen's ring cell (the test seam).
 *
 * @return the cell (0..29)
 */
uint8_t map_name_col_for_probe(void);

/** @brief Accessor: the ring angle [0x48] (the test seam).
 *
 * @return the angle
 */
uint16_t map_name_angle_for_probe(void);

/** @brief Set the ring angle (the test seam for the erase check).
 *
 * @param angle the new [0x48]
 */
void map_name_set_angle_for_probe(uint32_t angle);

/** @brief Accessor: the opening step (the test seam).
 *
 * @return the 1000:021E..0x0285 step
 */
uint8_t map_intro_step_for_probe(void);

/** @brief Accessor: the Enter sequence's phase (the test seam).
 *
 * @return 0 = fading to white, 1 = fading back, 2 = the header is up
 */
uint8_t map_enter_step_for_probe(void);

/** @brief Accessor: the Enter spin's BX counter (the test seam).
 *
 * @return the 0x04B9/0x0515 counter
 */
uint16_t map_enter_bx_for_probe(void);

/** @brief Accessor: the offscreen list page (the test seam).
 *
 * @return the 1000:8A4-style page buffer
 */
uint8_t *map_off_page_for_probe(void);

/** @brief Accessor: one typed-name display entry (the test seam; the [0x596]
 * entry).
 *
 * @param i the glyph index
 * @param tx the target x
 * @param ty the target y
 * @param cx the current x
 * @param cy the current y
 * @return 1 when the index exists
 */
int32_t map_glyph_for_probe(uint32_t i, int32_t *tx, int32_t *ty, int32_t *cx,
                            int32_t *cy);

/** @brief Accessor: the name-entry fire counter (the test seam).
 *
 * @return the [0x5AC8] model
 */
uint16_t map_name_fire_wait_for_probe(void);

/** @brief Accessor: the typed name buffer (the test seam).
 *
 * @return the name string
 */
const char *map_name_for_probe(void);

/** @brief Accessor: the staging page is filled (the test seam).
 *
 * @return 1 when ready
 */
uint8_t map_stage_ready_for_probe(void);

/** @brief Accessor: one staging-page byte (the test seam; 1000:12D3 plane byte).
 *
 * @param addr the byte address
 * @param plane the plane (0..3)
 * @return the byte, or 0 out of range
 */
uint8_t map_stage_at(uint32_t addr, uint32_t plane);

#endif /* HUMANSRE_MAP_H */
