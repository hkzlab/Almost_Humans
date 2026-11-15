/* The MAP screen's letter wheel: apl\alpha.spr, rotate_spinner_cell, the
 * 30-cell ring and the typed name (1000:0705/0F93/0CBE).
 */

#ifndef HUMANSRE_MAP_WHEEL_H
#define HUMANSRE_MAP_WHEEL_H

#include <stdint.h>

/** @brief 1000:0705 expand_nibbles_4plane: the packed 4bpp sheet into four planes of
 * 0x4000 bytes (a literal port of the bit twiddling).
 *
 * @param src the source buffer
 * @param dst the destination buffer
 */
void alpha_expand(const uint8_t *src, uint8_t *dst);

/** @brief 1000:0F93: the ring angle -> (destination byte, source offset).
 *
 * The x is `157 + 132*cos` px, the y `79 + 124*sin` quarter-rows; the page
 * stride is 80 and the `+0xF0` adds three rows.
 *
 * @param cell the map cell
 * @param angle the rotation angle
 * @param src the source offset (out)
 * @param dst the destination byte offset (out)
 */
void alpha_cell(uint32_t cell, uint32_t angle, uint16_t *src, uint16_t *dst);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Draw the letter wheel.
 *
 * @param page the page buffer
 * @param base the base offset
 */
void alpha_draw_wheel(uint8_t *page, uint32_t base);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief 1000:0CBE init_display_lists' name pass, stateless: the typed
 * characters at their final row-0x45 slots.
 *
 * @param page the page buffer
 * @param name the name
 */
void alpha_draw_name(uint8_t *page, const char *name);
#endif /* HUMANSRE_TEST_PROBES */

/**
 * @brief 1000:0CBE: clear the previous frame's cells, draw the 30-cell ring at
 * the current angle and the typed name at its display-list positions.
 *
 * @param page the page buffer
 */
void map_draw_name(uint8_t *page);

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
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

/** @brief Accessor: the typed name buffer (the test seam).
 *
 * @return the name string
 */
const char *map_name_for_probe(void);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_MAP_WHEEL_H */
