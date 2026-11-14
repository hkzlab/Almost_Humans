/* The terrain spawn walk and the 0xa9 log queue
 */

#ifndef HUMANSRE_OBJECT_SPAWN_H
#define HUMANSRE_OBJECT_SPAWN_H

#include <stdint.h>


/**
 * @brief Accessor: the 0x32B9 background-sprite queue's count (1000:5F54's
 * input, read by level_draw_bg_sprites).
 *
 * @return the background record count
 */
uint8_t object_spawn_bg_count(void);


/**
 * @brief Accessor: one background decoration's position.
 *
 * @param i the index
 * @param x the output x
 * @param y the output y
 */
void object_spawn_bg_at(uint32_t i, uint32_t *x, uint32_t *y);


/**
 * @brief 1000:5662: the style-6 +3 row nudge for the cell at (col, row).
 *
 * Every handler that stores a tile-aligned y calls it first: the terrain
 * spawns (0x52A2/0x52C7/0x52EB, 0x5481, 0x53BA ...) and the exit-marker
 * stamper (0x54A9), which runs it before `ADD DX,0x10 / SUB DX,0x40`.  The
 * lookup cell is the one directly below the matched tile; only a level of
 * style 6 has the table.
 *
 * @param col the cell column
 * @param row the cell row
 * @return 3 when the nudge applies, 0 otherwise
 */
uint32_t object_spawn_style_y_nudge(uint32_t col, uint32_t row);


/**
 * @brief Spawn the map's placed objects.
 *
 * load_terrain_b's per-tile object walk (1000:5168): [0x3E73] = 8, the 20
 * behaviour bytes cleared, then every map cell of the 25 table tiles in table
 * order (0xAA the human into the first free static slot, frame 0x10, type 2;
 * 0xAB the spear, appended, frame 0x60, +8 = 0xFFFF, behaviour 5); returns
 * [0x3E73].
 *
 * @param map the 640-cell map
 * @return the live record count after the spawn
 */
uint32_t object_spawn_load_terrain_objects(uint8_t *map);

#endif /* HUMANSRE_OBJECT_SPAWN_H */
