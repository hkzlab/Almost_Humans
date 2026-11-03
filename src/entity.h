/* entity.h — the interface of src/entity.c: entity table/bytecode (subsystem 10, §12)
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
    uint16_t x0; /* +0x0: endpoint A — handler 1's target        */
    uint16_t x1; /* +0x2: endpoint B — handler 0's target        */
    uint16_t handler; /* +0x4: the dispatch index (game: 0/1 only)    */
    uint16_t x; /* +0x6: the live position                      */
    uint16_t y; /* +0x8: row*16 - 0x30 + 1                      */
    uint16_t hold; /* +0xA: the beat countdown, seeded 3           */
    uint16_t waypoint; /* +0xC: the waypoint / animation index         */
    uint16_t life; /* +0xE: the goal word's high byte              */
} Entity;

int32_t entity_init(const OSystem *osys);
void entity_tick(void);
/* 1000:5823 / 0x5894: the entity sprite pass over apl\rex.dat (mode 2, two
 * 0x20-px cells per entity), and the three big-endian frame-table reads it
 * uses (1000:58A5).  Returns 1 when the index is inside the bank. */
void entity_publishBank(const uint8_t *bank, uint32_t size);
void entity_render(uint8_t *page, int32_t scrollX, int32_t scrollY);
int32_t entity_frameInfo(uint32_t a, uint32_t *src, uint32_t *rows,
                         uint32_t *yoff);

/* load_terrain_b (1000:5168): the 25-pair map walk in table order, one
 * handler call per matching cell (CX = col*16, DX = row*16; the spawn's
 * y is row*16 - 0x30 + 1, +3 under the style-6 prologue).  Resets the
 * count first (the level_init analog: the count only — the fixed-table
 * slots keep the exe image's values).  `style` = the record's +0x00 byte
 * (the prologue's [0x17EE]); `lifeSeed` = the goal word's high byte
 * ([0x17F1] -> the +0xE seed).  0 = ok, -1 = no map. */
int32_t entity_spawnFromMap(const uint8_t *map, uint32_t style,
                            uint32_t lifeSeed);

const Entity *entity_at(uint32_t i); /* NULL past the count */
Entity *entity_atMut(uint32_t i); /* the 0x58D1 collision pass writes */
int32_t entity_spritePixel(uint32_t src, uint32_t rows, uint32_t x, uint32_t y);
uint32_t entity_count(void);
/* save +0x1656+0x28: [0x64C2] and the first seven 0x10-byte records. */
void entity_saveFixed(uint8_t *dst);
void entity_loadFixed(const uint8_t *src);

#endif /* HUMANSRE_ENTITY_H */
