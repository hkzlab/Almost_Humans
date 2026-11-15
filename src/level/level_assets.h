/* The level's visual assets: the BD page walk, the tile atlas, the
 * background, the terrain page tables and the exit stamps.
 */

#ifndef HUMANSRE_LEVEL_ASSETS_H
#define HUMANSRE_LEVEL_ASSETS_H

#include <stdint.h>

/** @brief page_offset_to_ptr (1000:F123): 1 when the tile's page bytes are free.
 *
 * That is: the 16-byte page mask is all-negative (every sub-cell empty) AND the
 * 16-word item record sums to 0 (16-bit wrap, as DX).  The caller (op 3) bails
 * on a non-zero return.
 *
 * @param id the identifier
 * @return 1 when the tile's page bytes are free, 0 otherwise
 */
uint8_t level_page_free(uint32_t id);

/**
 * @brief Release the tile atlas once the level's palette is built.
 *
 * Nothing reads it after `_level_palette_build`; the original loads
 * `APL\SPRITES.DAT` over it in the same `[0x13A9]` scratch segment.
 */
void level_assets_release_atlas(void);

/* The two level maps the walk reads: `0x92A0`'s page bytes (bit 7 = solid, the
 * low 7 bits the step, 1000:E389) and `[0x1FD6]`'s 16-word item records
 * (1000:F25F's occupancy bit). */

/** @brief Field K, the level's four special tile ids.
 *
 * i = 0/1 are the first word's two tile ids ([0x6F20]/[0x6F21], compared with
 * the cell under the feet), i = 2/3 the second word's ([0x6F0B]/[0x6F0C];
 * 1000:4E99, the fall machine's type compare at 1000:F7CD and 1000:E30E).
 *
 * @param i the index
 * @return the special tile id, or 0xFFFF when out of range
 */
uint32_t level_special_tile(uint32_t i);

/** @brief The `0x92A0` page mask's byte for a tile id and sub-column: bit 7 = solid,
 * the low 7 bits are the step 1000:E389 follows.  `level_page_free` reads the
 * same table for op 3.
 *
 * @param id the identifier
 * @param subcol the sub-column
 * @return the page mask byte
 */
uint32_t level_page_byte(uint32_t id, uint32_t subcol);

/**
 * @brief The map cell's item word (tile row and sub-column).
 *
 * @param id the identifier
 * @param row the row
 * @return the item word, or 0xFFFF when out of range
 */
uint32_t level_item_word(uint32_t id, uint32_t row);

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/** @brief The item record's raw byte at `off` from the tile's offset, i.e. from field
 * B.  (`1000:EC27` reads field A, the slope profiles, through
 * `level_slope_byte`; this accessor is the item record's own field-B read.)
 *
 * @param id the identifier
 * @param off the offset
 * @return the item byte, or 0x80 when out of range
 */
uint32_t level_item_byte(uint32_t id, uint32_t off);
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief The stamped background, for the exit/igloo test.
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_bg_for_probe(void);

/**
 * @brief Whether the tile atlas is still resident.
 *
 * @return 1 while loaded, 0 after `level_assets_release_atlas`
 */
uint8_t level_atlas_live_for_probe(void);
#endif /* HUMANSRE_TEST_PROBES */

/** @brief 1000:54A9's stamp y for the exit cell at (col, row): the handler's
 * `row*16`, then 0x5662's style-6 nudge, then `ADD DX,0x10 / SUB DX,0x40`.
 *
 * @param col the cell column
 * @param row the cell row
 * @return the stamped block's top y
 */
int32_t level_exit_stamp_y(uint32_t col, uint32_t row);

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
/**
 * @brief Accessor: the record's P block (exit art).
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_p_block_for_probe(void);

/**
 * @brief Accessor: the loaded background index.
 *
 * @return the index
 */
uint16_t level_bg_index_for_probe(void);

/**
 * @brief Accessor: the record's C block (0xA9 background art).
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_c_block_for_probe(void);
#endif /* HUMANSRE_TEST_PROBES */

/** @brief 1000:5F54 - render_objects' tail: for each 0x32B9 entry it blits the two
 * 32 x 24 mode-2 C-block sprites at (x, y) and (x + 0x20, y) (0x5FBE ADDs
 * 0x300 to the source and 0x20 to x for the second).  The queue holds one
 * entry per 0xA9 cell, and all 14 shipped levels with a 0xA9 have exactly
 * one.  The original's drawer steps DI by 8 (not the record's 12) at
 * 1000:5FD2, so with more than one entry it reads the first record's +8/+A
 * as x/y; nothing shipped reaches that.  The page here is the flat-compose
 * window, so the view is subtracted exactly as the object passes do.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void level_draw_bg_sprites(uint8_t *page, int32_t scrollX, int32_t scrollY);

/**
 * @brief Accessor: the level's [0x6532] (BD field F, 0 when absent).
 *
 * @return the animation frame count
 */
uint32_t level_anim_frames(void);

/**
 * @brief 1000:1FD3 field A: the per-tile signed slope profile at `sub`
 * (behaviour 4, signed -4..+4).
 *
 * @param id the identifier
 * @param sub the sub-column
 * @return the signed slope byte
 */
int32_t level_slope_byte(uint32_t id, uint32_t sub);

#endif /* HUMANSRE_LEVEL_ASSETS_H */
