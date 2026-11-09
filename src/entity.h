/* entity.h - the interface of src/entity.c: entity table/bytecode (subsystem 10, §12)
 *
 * The ported, closed §12.6 system: tile 0xB3 spawns a dinosaur, a 0xB4 on
 * the same row supplies the far endpoint (entity_find_by_id), and the
 * game itself alternates the dispatched handler (the shared tail's +4 ^=
 * 1 flip) so the dino paces between the two endpoints at ±1 px per tick.
 * All field meanings and the per-tick machine are in re_formats §12.6.
 */

#ifndef HUMANSRE_ENTITY_H
#define HUMANSRE_ENTITY_H

#include "osystem.h"

/* One entity record: the DS layout word-for-word (+0x00..+0x0E). */
typedef struct {
    uint16_t x0; /* +0x0: endpoint A - handler 1's target        */
    uint16_t x1; /* +0x2: endpoint B - handler 0's target        */
    uint16_t handler; /* +0x4: the dispatch index (game: 0/1 only)    */
    uint16_t x; /* +0x6: the live position                      */
    uint16_t y; /* +0x8: row*16 - 0x30 + 1                      */
    uint16_t hold; /* +0xA: the beat countdown, seeded 3           */
    uint16_t waypoint; /* +0xC: the waypoint / animation index         */
    uint16_t life; /* +0xE: the goal word's high byte              */
} Entity;

/**
 * @brief --- the interface ------------------------------------------------------
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t entity_init(const OSystem *osys);

/**
 * @brief --- the dispatch (1000:C93C) -------------------------------------------
 */
void entity_tick(void);

/**
 * @brief Publish the REX.DAT frame bank.
 *
 * 1000:5823 / 0x5894: the entity sprite pass over apl\rex.dat (mode 2, two
 * 0x20-px cells per entity), and the three big-endian frame-table reads it
 * uses (1000:58A5).
 * Returns 1 when the index is inside the bank.
 *
 * @param bank the sprite bank
 * @param size the size
 */
void entity_publish_bank(const uint8_t *bank, uint32_t size);

/** @brief 1000:5823 / 0x5894: the entity pass.  Runs after the placed records
 * (render_objects 0x5D56 calls 0x6421 then 0x5823), before the bar.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void entity_render(uint8_t *page, int32_t scrollX, int32_t scrollY);

/** @brief The three table reads for one cell index; 0 = the indices are outside the
 * bank (the original would read whatever follows it in memory).
 *
 * @param a the frame index
 * @param src the source buffer
 * @param rows the row count
 * @param yoff the y offset
 * @return 1 when the indices are inside the bank, 0 otherwise
 */
uint8_t entity_frame_info(uint32_t a, uint32_t *src, uint32_t *rows,
                          uint32_t *yoff);

/** @brief One pass over the whole 32x20 map per table pair, in table order, the
 * handler invoked at each matching cell with CX = col*16, DX = row*16
 * (the SHL 4s at 51bf-51cd).  The scan is column-inner, row-outer
 * ([0x4234] = cols 0x1F..0, [0x4230] = rows 0x13..0).
 *
 * load_terrain_b (1000:5168): the 25-pair map walk in table order, one handler
 * call per matching cell (CX = col*16, DX = row*16; the spawn's y is row*16 -
 * 0x30 + 1, +3 under the style-6 prologue).
 * Resets the count first (the level_init analog: the count only - the
 * fixed-table slots keep the exe image's values).
 * `style` = the record's +0x00 byte (the prologue's [0x17EE]); `lifeSeed` =
 * the goal word's high byte ([0x17F1] -> the +0xE seed).
 * 0 = ok, -1 = no map.
 *
 * @param map the 640-cell map
 * @param style the level style
 * @param lifeSeed the life seed
 * @return 0 on success, -1 when the map is missing
 */
int32_t entity_spawn_from_map(const uint8_t *map, uint8_t style,
                              uint8_t lifeSeed);

/**
 * @brief Accessor: one entity record, NULL past the count.
 *
 * @param i the index
 * @return the record, or NULL past the count
 */
const Entity *entity_at(uint32_t i);

/**
 * @brief the mutable entity record (the 1000:58D1 collision pass writes it).
 *
 * @param i the index
 * @return the pointer, or NULL when unavailable
 */
Entity *entity_at_mut(uint32_t i);

/** @brief 1000:58D1's sprite-pixel test: the REX bank pixel at the (x,y) offset in
 * the sprite whose descriptor entity_frame_info(a) returned.
 *
 * @param src the source buffer
 * @param rows the row count
 * @param x the x coordinate
 * @param y the y coordinate
 * @return 1 when the pixel is non-zero, 0 otherwise
 */
uint8_t entity_sprite_pixel(uint32_t src, uint32_t rows, uint32_t x,
                            uint32_t y);

/**
 * @brief Accessor: the live entity count.
 *
 * @return The live entity count.
 */
uint16_t entity_count(void);

/** @brief Save +0x1656+0x28 = [0x64C2] (u16) + the first seven 0x10-byte records
 * (0x64C4..0x6534).  The original copies 0x70 bytes, i.e. slots 0..6.
 *
 * @param dst the destination buffer
 */
void entity_save_fixed(uint8_t *dst);

/**
 * @brief Restore the entity table from a save.
 *
 * @param src the source buffer
 */
void entity_load_fixed(const uint8_t *src);

#endif /* HUMANSRE_ENTITY_H */
