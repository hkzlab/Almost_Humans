/* The level record: LEVELS\L*.BIN parsing, the header and map,
 * and the live writable map copy.
 */

#ifndef HUMANSRE_LEVEL_LOAD_H
#define HUMANSRE_LEVEL_LOAD_H

#include <stdint.h>

typedef struct {
    uint16_t levelId; // 0..159
    uint8_t style; // +0x00 low: the tile style 1..6
    uint8_t introIndex; // +0x01 high: the intro-movie index (0xFF=none)
    uint8_t goalCount; // +0x02 low: the solved-flag threshold
    uint8_t entityLife; // +0x02 high: the entity lifetime seed
    uint16_t objectTest; // +0x04: the object-test mode word
    uint8_t unused06; // +0x06: 0 in all 160 shipped records
    uint16_t timeA; // +0x07: the BCD MMSS limit (difficulty A)
    uint16_t timeB; // +0x09: difficulty B
    uint16_t timeC; // +0x0B: difficulty C
    uint8_t map[640]; // 32 columns x 20 rows, row-major
    uint32_t mapFileOff; // the map's offset inside the L*.BIN image
    char goal[128]; // the goal text (CR = a line break)
    char author[128]; // the author text
} LevelData;

/**
 * @brief Level module init (the backend is bound once by osys_bind()).
 *
 * @return 0 on success
 */
int32_t level_init(void);

/**
 * @brief Load one level record and its terrain from the volume (0..159; 0 = ok).
 *
 * @param levelId the level id
 * @return 0 on success, -1 on failure
 */
int32_t level_load(uint16_t levelId);

/**
 * @brief Accessor: the loaded LevelData record.
 *
 * @return the record, or NULL before a load
 */
const LevelData *level_data(void);

/**
 * @brief Accessor: the live 640-cell map buffer.
 *
 * The loader's writable copy of LevelData.map (1000:3138).  In play
 * object_spawn_load_terrain_objects and the 0xA9 handler rewrite it; the record's
 * own map (level_data()->map) stays the pristine parsed copy.
 *
 * @return the map buffer, or NULL before a load
 */
const uint8_t *level_map(void);

/**
 * @brief The record's style byte (header +0, `[0x17EE]`): 1000:5662 tests it against 6.
 *
 * @return the level style
 */
uint32_t level_style(void);

#endif /* HUMANSRE_LEVEL_LOAD_H */
