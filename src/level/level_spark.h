/* The claim's particles (1000:3E73/0x3FB3/0x3F4A) and the frozen claim
 * frames.
 */

#ifndef HUMANSRE_LEVEL_SPARK_H
#define HUMANSRE_LEVEL_SPARK_H

#include <stdint.h>

/** @brief 1000:3D5E: the claim's pan frames recolour every page byte below 0x80 to
 * 0x1E before the present (the original's four plane passes are one flat
 * page here). The planar shadow is recoloured too: it is what the spark's
 * strip save and the frozen frames compose from.
 */
void level_claim_recolour(void);

/** @brief the claim's frozen page (1000:3D04 -> 1000:3CB5).
 *
 * The original draws the world once, recolours it black on the pan, then only
 * presents the spark strip plus the selected/claimed records.  In the flat
 * model that is 1000:40CA's two records + 1000:3F4A's spark over the recoloured
 * page, never a world re-compose, plus the bar (outside the recoloured page).
 */
void level_claim_frame(void);

/**
 * @brief 1000:3E73's tail (`JMP 0x3DC0` at (x - 0x20, y - 0x20)): clear the
 * list, take the shade table and the spark origin, and save the strip.
 *
 * The claim's particles (1000:3E73/0x3FB3/0x3F4A): begin at the appended
 * record with the 16x16 shade table object.c builds from the recipe sprite.
 *
 * @param ox the spark origin x
 * @param oy the spark origin y
 * @param shade the 16x16 shade table
 */
void level_spark_begin(uint16_t ox, uint16_t oy, const uint8_t *shade);

/**
 * @brief Accessor: the spark effect is running.
 *
 * @return 1 while running, 0 otherwise
 */
uint8_t level_spark_active(void);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Test seam: one byte of 1000:3DC0's scratch strip.
 *
 * @param col the strip address column (0..0x11)
 * @param row the strip row (0..0x3F)
 * @param plane the plane byte (0..3)
 * @return the byte, or 0 out of range
 */
uint8_t level_spark_strip_at(uint32_t col, uint32_t row, uint32_t plane);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief The first live spark slot's world position and value (the test seam).
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param value the value
 * @return 1 when a live spark slot was found, 0 otherwise
 */
int32_t level_spark_peek(uint32_t *x, uint32_t *y, uint32_t *value);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief The 0x48BC queue as the original's 0x2C0-byte window (offset 0 =
 * 0x48BC), for object.c's DS record model.
 *
 * @param off the offset
 * @return the spark byte
 */
uint8_t level_spark_byte(uint32_t off);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Test seam: poke one spark buffer byte.
 *
 * @param off the offset
 * @param value the value
 */
void level_spark_set_byte(uint32_t off, uint8_t value);
#endif /* HUMANSRE_TEST_PROBES */

#endif /* HUMANSRE_LEVEL_SPARK_H */
