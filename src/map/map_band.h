/* The MAP screen's band: BAND.SPR's box frame and its four-strip animation
 * (1000:12D3/07E6/0A72/09DA).
 */

#ifndef HUMANSRE_MAP_BAND_H
#define HUMANSRE_MAP_BAND_H

#include <stdint.h>

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

/** @brief 1000:07B2 palette_reset: seed the shadows and animate twice (the
 * original's two planar page copies are the flat page itself in the port).
 */
void map_palette_reset(void);

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
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
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_MAP_BAND_H */
