/* entity.c - the entity table and its per-tick dispatch
 *
 * Ported routines:
 *
 *   entity_dispatch         1000:C93C  the count walk + the indexed call
 *   entity_advance_fixed_e4 1000:C96C  handler 0: latches the 0x64E4 words
 *   entity_advance_fixed_f8 1000:C973  handler 1: latches the 0x64F8 words
 *   entity_waypoint_tail    1000:C9C1  one walk tick + the endpoint flip
 *   entity_append_to_array  1000:5360  the spawn (tile 0xB3, table #10)
 *   entity_find_by_id       1000:5399  the join (tile 0xB4, table #11)
 *   load_terrain_b          1000:5168  the 25-pair map walk (per level load)
 *   style_handler_prologue  1000:5662  the style-6 y+3 spawn test
 *
 * The waypoint machine, one tick of which the tail runs (all per the
 * listing; the state a fresh handler call latches is marked):
 *   compare/primary/step triple at the handler's fixed base, re-latched
 *   EVERY tick; primary -> [0x423C], step -> [0x4238]; the walk branch adds
 *   the step to x and flips the dispatch index at an exact endpoint hit;
 *   the non-arrival path counts the +0xA hold down and walks the +0xC
 *   waypoint, which snaps back to the primary when it reaches primary+8 -
 *   an 8-frequency animation clock.  Nothing here reads the map or the
 *   video.  The sprite draw is the REX.DAT consumer, ported below:
 *   `entity_render` (1000:5823 -> 0x5894 -> 0x58A5 -> 0x5C5F), two mode-2
 *   32-px cells per entity from the bank's three big-endian frame tables.
 *
 * The tail's interaction pass (0xCA43..CB77) is ported as
 * `_entity_tail_interact`: it walks the placed-object array (0x3E75), tests the
 * +/-x/+/-y overlap windows and posts the type-0x22 carry record with a call to
 * the sound driver (9).  It writes the entity's handler/waypoint/hold, which
 * is what makes handlers 2 and 3 (the carry animation) reachable.
 */
#include "entity.h"
#include "object.h"
#include "osystem.h"
#include "score.h"
#include "byteorder.h"
#include "width.h"

#include <stddef.h>
#include <string.h>

/* --- the record and the array space ------------------------------------ */
#define ENT_MAX             12u
#define MAP_COLS            32u
#define MAP_ROWS            20u
#define TILE_HANDLER_APPEND 10u   // 0xB3 -> entity_append_to_array 5360
#define TILE_HANDLER_FIND   11u   // 0xB4 -> entity_find_by_id 5399

/* The DS globals 0x64C4.. (the exe image at file 0x106F0 + 0x64C4,
 * byte-verified 2026-10-23).  Slots 2 and 3 are not just array slots:
 * handlers 0 and 1 read them as their fixed word tables, and
 * level_init (1000:4CAB) zeroes ONLY the count [0x64C2], so the image's
 * values survive to runtime - this is how a level with up to two 0xB3
 * tiles walks on tables it never wrote (live-verified: the injected single
 * record walked on the baked words; live table +0x4 = 0/1). */
static uint16_t _ents[ENT_MAX][8] = {
    /* 0x64C4: slot 0 - the appender's first record (image: zero)       */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* 0x64D4: slot 1 - the appender's second record (image: zero)      */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* 0x64E4: slot 2 - handler 0's fixed words (compare, primary, step,
     * then the walk tail the pair reader walks past)                   */
    { 0x000C, 0x0000, 0x0001, 0xFFDD, 0x0020, 0x000A, 0x0002, 0x0025 },
    /* 0x64F4: slot 3 - handler 1's fixed words start at w[2]           */
    { 0x0003, 0x0008, 0x001A, 0x000E, 0xFFFF, 0x0010, 0x001F, 0x0018 },
    /* 0x6504: slot 4 - the tail of the baked words; appender slot 4
     * starts here (the image is zero from 0x650C)                      */
    { 0x0000, 0xFFF0, 0x0002, 0x0016, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* 0x6514 .. 0x6534: image-zero padding to the end of the reservation */
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
};
static uint16_t _ent_count;    // [0x64C2]

/* The dispatcher's shared DS words: [0x4238] = the secondary selector
 * (the movement step this tick), [0x423C] = the primary selector. */
static uint16_t _sel_secondary;  // [0x4238]
static uint16_t _sel_primary;    // [0x423C]

static uint8_t _cur_style;      // [0x17EE]: the level header's style

/* --- load_terrain_b's data (the exe image, byte-verified) --------------- */

/* 202F:0x1EB1 = the pair count (25); 0x1EB3 = the tile-id bytes; 0x1ECC =
 * the handler offsets.  The port runs only the two handlers that touch
 * the entity array - table #10 (0xB3 -> the appender) and #11 (0xB4 ->
 * the finder); the other 23 handlers spawn placed objects / write the
 * 0x64A2..0x64BA globals and land with object.c. */
static const uint8_t _tile_ids[25] = {
    0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1,
    0xB2, 0xB3, 0xB4, 0xB5, 0xC0, 0xB6, 0xB7, 0xB8, 0xBA,
    0xBB, 0xBE, 0xBF, 0xC1, 0xF0, 0xF1, 0xF2,
};

/* The 0x1F20 byte flag table (image 0x1F20..0x1F3F), the style-6 spawn
 * test's bit source (1000:567d). */
static const uint8_t _style6_flags[32] = {
    0x00, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x07, 0xE0, 0x00, 0x02,
    0x02, 0x00, 0x03, 0xC3, 0x81, 0x80, 0x01, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* --- the REX.DAT frame tables (1000:58A5) ------------------------------
 *
 * `update_object_list` (1000:5823) walks the entities, sets `ES` to the
 * `apl\rex.dat` slot and calls `draw_objects_all` (1000:5894) with the entity's
 * x - 0xC, its y, its waypoint as the animation index and mode 2.  That routine
 * doubles the index (`SHL CX,1`) and draws TWO cells 0x20 px apart, whose index
 * `a` is `2*waypoint + k`.  For each cell the bank is read through three
 * BIG-ENDIAN tables (the file is a byte-swapped export):
 *
 *   BE16(4a + 2)      the block offset; the sprite data starts 0x1C0 into it
 *   BE16(2a + 0xE0)   a y offset added to the entity's y
 *   BE16(2a + 0x150)  the row count
 *
 * and the cell is blitted with mode 2 (`1000:5C5F` -> `1000:6C74`): 32 px wide,
 * 8 source bytes per row per plane pass - the same storage law as the object
 * sprites (`block = rows * 8`).
 */
static const uint8_t *_ent_bank;
static uint32_t _ent_bank_size;

/* --- the appender (1000:5360) and the prologue (1000:5662) -------------- */

/** @brief style_handler_prologue (1000:5662): the walked record's style is 6 and
 * the map cell DIRECTLY BELOW the match (matchIdx + 0x20 - the +0x1F the
 * prologue reads is offset from the walker's cursor, which has already
 * stepped +1 past the match) is <= 0x90 and passes the 0x1F20 flag bit
 * 0x80>>(v&7): y gains 3 px.  Everything (the finder too) pre-calls this,
 * before the -0x30+1.  A row-19 match reads past the 640-byte map copy
 * into the original's DS neighbours - not reproducible from the map; the
 * corpus has no B3/B4 on row 19, so the port yields 0 there (the guard
 * keeps the read in bounds).
 *
 * @param map the 640-cell map
 * @param mapLen the map length
 * @param matchIdx the matched map index
 * @return 1 when the cell carries the style-6 bonus, 0 otherwise
 */
static int32_t _style6_y_bonus(const uint8_t *map, uint32_t mapLen,
                               uint32_t matchIdx)
{
    if (_cur_style != 6u) return 0;
    if (matchIdx + 0x20u >= mapLen) return 0;
    uint8_t v = map[matchIdx + 0x20u];
    if (v > 0x90u) return 0;
    uint8_t flag = _style6_flags[(v >> 3) & 31u];
    /* MOV AX,1 / SHL AX,CL with CL = 7-(v&7) - bit 0x80>>(v&7) */
    if ((flag >> (7u - (v & 7u))) & 1u) return 1;
    return 0;
}

/** @brief entity_append_to_array (1000:5360..5398).  Summoned per 0xB3 cell in
 * map scan order.
 *
 * @param map the 640-cell map
 * @param mapLen the map length
 * @param matchIdx the matched map index
 * @param cx the x value
 * @param dx the x delta
 * @param lifeSeed the life seed
 */
static void _entity_append(const uint8_t *map, uint32_t mapLen,
                           uint32_t matchIdx, uint32_t cx, uint32_t dx,
                           uint32_t lifeSeed)
{
    if (_style6_y_bonus(map, mapLen, matchIdx)) dx += 3;   // 1000:5698

    if (_ent_count >= ENT_MAX) {
        /* The original would write past the DS reservation (corrupting
         * the 0x6514-onwards image bytes); the corpus peak is 9 per
         * level, so the port bound is never hit in shipped data. */
        _ent_count = ENT_MAX;
        return;
    }
    uint16_t *e = _ents[_ent_count++];
    uint16_t dxAdj = (uint16_t)(dx + 1u - 0x30u);         // 5373/5376
    e[0] = (uint16_t)cx;          // +0 = the spawn column * 16
    e[1] = 0;                     // +2 = 0 (a 0xB4 writes the far end)
    e[2] = 0;                     // +4 = the dispatch index: seeded 0
    e[3] = (uint16_t)cx;          // +6 = current x = the same word
    e[4] = dxAdj;       // +8 = the spawn row
    e[5] = 3;                     // +A = the beat countdown seed
    e[6] = 0;                     // +C = the waypoint
    e[7] = (uint16_t)lifeSeed;    // +E = [0x17F1] = the goal high byte
}

/** @brief entity_find_by_id (1000:5399..53b8).  Summoned per 0xB4 cell: rewrites
 * the FIRST entity whose spawn row (+8) matches this row's x1 endpoint.
 *
 * @param map the 640-cell map
 * @param mapLen the map length
 * @param matchIdx the matched map index
 * @param cx the x value
 * @param dx the x delta
 */
static void _entity_find(const uint8_t *map, uint32_t mapLen, uint32_t matchIdx,
                         uint32_t cx, uint32_t dx)
{
    if (_style6_y_bonus(map, mapLen, matchIdx)) dx += 3;   // 1000:5698
    uint16_t match = (uint16_t)(dx + 1u - 0x30u);         // 53a5/53a8

    for (uint16_t i = 0; i < _ent_count; i++) {
        if (_ents[i][4] != match) continue;      // 53a9
        _ents[i][1] = (uint16_t)cx;                        // 53ae
        return;                                           // 53b1
    }
}

/* --- load_terrain_b (1000:5168): the level's map walk ------------------- */

/**
 * @brief Spawn every map cell that matches one table tile.
 *
 * @param pair the table pair
 * @param tile the tile byte
 * @param map the 640-cell map
 * @param lifeSeed the life seed
 */
static void _spawn_tile_pair(uint32_t pair, uint8_t tile, const uint8_t *map,
                             uint32_t lifeSeed)
{
    for (uint16_t row = 0; row < MAP_ROWS; row++) {
        for (uint16_t col = 0; col < MAP_COLS; col++) {
            uint16_t idx = row * MAP_COLS + col;
            uint16_t cx = col * 16u;
            uint16_t dx = row * 16u;

            if (map[idx] != tile) continue;
            switch (pair) {
            case TILE_HANDLER_APPEND:
                _entity_append(map, MAP_COLS * MAP_ROWS, idx, cx, dx, lifeSeed);
                break;
            case TILE_HANDLER_FIND:
                _entity_find(map, MAP_COLS * MAP_ROWS, idx, cx, dx);
                break;
            default:
                /* the placed-object spawners: with object.c */
                break;
            }
        }
    }
}

int32_t entity_spawn_from_map(const uint8_t *map, uint8_t style,
                              uint8_t lifeSeed)
{
    if (!map) return -1;

    /* level_init (1000:4CAB): the count and nothing else - the fixed
     * table slots keep their image values (the array's initialiser). */
    _ent_count = 0;
    _cur_style = style;
    _sel_secondary = _sel_primary = 0;

    for (uint16_t pair = 0; pair < 25u; pair++)
        _spawn_tile_pair(pair, _tile_ids[pair], map, lifeSeed);
    return 0;
}

/* --- the waypoint tail (1000:C9C1..CA44) -------------------------------- */

/* One tick of the walk.  `wp` = the handler's fixed triple (compare /
 * primary / step) freshly latched from the baked slots, `endX` = the
 * handler's endpoint word (DX: handler 0 passes x1, handler 1 passes
 * x0).  Everything below is the listing, step for step. */
/** @brief The handler tables are contiguous in DS (0x64E4 / 0x64F8, eight words per
 * record), and the tail reads past its own row - 1000:CB0E onward consumes
 * wp[5..9], which for handler 0 runs into slot 3.  The port therefore addresses
 * them flat.
 *
 * @param slot the slot
 * @param off the offset
 * @param k the loop counter
 * @return the table word, or 0 past the table
 */
static uint16_t _ent_word(uint32_t slot, uint32_t off, uint32_t k)
{
    const uint16_t *flat = &_ents[0][0];
    uint16_t i = (uint16_t)(slot * 8u + off + k);

    return i < ENT_MAX * 8u ? flat[i] : 0u;
}

/** @brief The entity's index, for the +0xA carry link (the port's stand-in for the
 * original's DS pointer 0x64C4 + 0x10*n).
 *
 * @param ep the entity record
 * @return the entity's index
 */
static uint32_t _ent_index(const Entity *ep)
{ return (uint32_t)(ep - (const Entity *)_ents[0]); }

/** @brief 1000:CA43..CB77 - the tail's interaction pass, run on every walk-branch tick
 * that does not reach the endpoint.  It walks the eight static placed records
 * and takes the first one that
 *   - is active ([+6] bit 7 clear),
 *   - sits at `x - entity.x + wp[3]` in 0..2 and `y - entity.y - 0x14` in
 *     0..0x2C (wp[3] is the x tolerance: 0xFFDD = -35 for handler 0, +0x10
 *     for handler 1),
 *   - is not the type wp[4] names, not type 4/5, and not marked in 0x6EC3,
 * dissolves it (1000:F453 with DX = 5) and posts the carry: the record becomes
 * type 0x22, its +0xA the entity link, +0xE = wp[5], +0x10 = wp[7] + the
 * entity word wp[6] names; the entity's handler becomes wp[8], its waypoint
 * wp[9] and its hold 0xA032 (the "stuck while carrying" sentinel), effect 9.
 *
 * The type compare is an equality branch, not a skip (1000:CABC -> 1000:CB78):
 * a record whose +8 is exactly wp[4] (0x20 handler 0 / 0x1F handler 1) is the
 * guard stance op 5 (`entity_promote_to_type_1f` 1000:39B5) writes on a human
 * holding a spear or torch, and the original turns the entity around there.
 *
 * `+0xA` holds a DS pointer to the entity record in the original; the port has
 * no DS image, so it stores `entity index + 1` (0 = none) - the same internal
 * encoding policy the object links use.
 *
 * @param ep the entity record
 * @param slot the slot
 * @param off the offset
 */
static void _entity_tail_interact(Entity *ep, uint32_t slot, uint32_t off)
{
    uint16_t w3 = _ent_word(slot, off, 3), w4 = _ent_word(slot, off, 4);
    uint16_t w5 = _ent_word(slot, off, 5), w6 = _ent_word(slot, off, 6);
    uint16_t w7 = _ent_word(slot, off, 7), w8 = _ent_word(slot, off, 8);
    uint16_t w9 = _ent_word(slot, off, 9);

    for (uint16_t i = 0; i < 8u; i++) { // DX = 7: the static eight
        uint8_t *rec = object_slot(i);
        int32_t dx, dy;
        uint16_t type;

        if (!rec) return;
        if (rec[6] & 0x80u) continue; // 1000:CA6B
        dx = (int32_t)(int16_t)(uint16_t)(rd_le_u16(rec + 0u) - ep->x) +
             (int32_t)(int16_t)w3;
        if (dx < 0 || dx > 2) continue; // 1000:CA8A
        dy = (int32_t)(int16_t)(uint16_t)(rd_le_u16(rec + 2u) - ep->y) - 0x14;
        if (dy < 0 || dy > 0x2C) continue; // 1000:CAA4
        type = rd_le_u16(rec + 8u);
        if (type == w4) { // 1000:CABC -> 1000:CB78
            /* the guard reversal: the entity turns around instead of carrying
             * the guarding record */
            _sel_primary ^= 0x000Eu;
            ep->waypoint = _sel_primary;
            ep->handler ^= 1u;
            return;
        }
        if (type == 4u || type == 5u) continue; // 1000:CAC5
        if (object_type_blocked(type)) continue; // 1000:CAE1 (DS:0x6EC3)
        (void)object_stack_scan_dissolve(i); // 1000:CAFE: 0xF453, DX = 5
        wr_le_u16(rec + 8u, 0x0022u); // 1000:CB15
        /* 1000:CB19 stores the entity record pointer (0x64C4 + 0x10*n); the
         * port tags the index so it cannot collide with the object links
         * (4 + idx*0x12) the same +0xA field serves elsewhere. */
        wr_le_u16(rec + 0xAu, (uint16_t)(0x8000u | _ent_index(ep)));
        wr_le_u16(rec + 0xEu, w5); // 1000:CB23
        { // 1000:CB35..CB4C
            uint16_t idx = w6 / 2u; // the entity's word offset
            uint16_t base = 0;

            if (w6 <= 0xEu && !(w6 & 1u)) base = ((const uint16_t *)ep)[idx];
            wr_le_u16(rec + 0x10u, (uint16_t)(w7 + base));
        }
        ep->handler = w8; // 1000:CB56
        ep->waypoint = w9; // 1000:CB65
        ep->hold = 0xA032u; // 1000:CB6A
        object_driver_call(9); // 1000:CB73
        return;
    }
}

/**
 * @brief One entity walk tick and the endpoint flip.
 *
 * @param ep the entity record
 * @param slot the slot
 * @param off the offset
 * @param endX the endpoint x
 */
static void _entity_tail(Entity *ep, uint32_t slot, uint32_t off, uint16_t endX)
{
    uint16_t w0 = _ent_word(slot, off, 0);
    _sel_primary = w0; // c9c1/c9c5
    /* c9c8: ADD BP,2 - the walk branch reads the NEXT words          */

    if (w0 != ep->waypoint) { // c9cb/c9ce: the walk branch
        _sel_primary = _ent_word(slot, off, 1); // c9dd
        _sel_secondary = _ent_word(slot, off, 2); // c9e4/c9e8
        ep->x = (uint16_t)(ep->x + _sel_secondary); // c9ee/c9f1: ADD [DI],AX
        if (endX == ep->x) { // c9f3: the endpoint hit
            _sel_primary ^= 0x000Eu; // c9f7
            ep->waypoint = _sel_primary; // c9fc/c9ff
            ep->handler ^= 1u; // ca02/ca05: the ping-pong
            return;
        }
        /* ca09..ca3b: the beat (the hold countdown, then the waypoint step and
         * its 8-snap), and then CA43's interaction pass, which runs on BOTH
         * beat exits - ca18's JNS and the ca1a path both land on ca3f. */
        ep->hold = (uint16_t)(ep->hold - 1u);
        if ((int16_t)ep->hold < 0) { // ca0f/ca13/ca17/ca18
            ep->hold = 4; // ca1a
            ep->waypoint += 1u; // ca20
            _sel_secondary = (uint16_t)(_sel_primary + 8u); // ca24/ca27/ca2a
            if (_sel_secondary == ep->waypoint) // ca2f/ca32/ca36
                ep->waypoint = _sel_primary; // ca38/ca3b: the 8-snap
        }
        _entity_tail_interact(ep, slot, off); // ca3f -> ca43..cb77
        return;
    }

    /* c9d0: the beat branch (w0 == waypoint): hold down; on expiry the
     * waypoint takes the primary... the pair's +2 word (c9d5 uses [BP] at
     * the +2 position = wp[1]). */
    ep->hold = (uint16_t)(ep->hold - 1u);
    if ((int16_t)ep->hold < 0)
        ep->waypoint = _ent_word(slot, off, 1); // c9d5/c9d9
}

/** @brief 1000:CB99: the carry animation stepper handlers 2 and 3 jump into.  The
 * record's fields are used as BYTES here: +0xA is a 4-tick hold, +0xB the
 * animation sub-step (stepped down by 0x10) and +0xC a word frame counter that
 * walks up to the target in [0x423C].  On reaching it the target drops by 0xC
 * and the counter takes it; when the sub-step's 0x10 bit has gone, the handler
 * returns to the base in [0x4238] (0 for handler 3, 1 for handler 2) with the
 * hold cleared.  Effect 0xB plays on each 0x10 step.
 *
 * @param ep the entity record
 */
static void _entity_carry_anim(Entity *ep)
{
    uint8_t *b = (uint8_t *)ep;
    uint16_t frame;

    b[0xAu] = (uint8_t)(b[0xAu] - 1u);
    if ((int8_t)b[0xAu] >= 0) return; // cb9e/cba2
    b[0xAu] = 4; // cba4
    {
        uint8_t sub = b[0xBu]; // cbaa

        frame = (uint16_t)((rd_le_u16(b + 0xCu) + 1u)); // cbb3: INC word [BP]
        wr_le_u16(b + 0xCu, frame);
        if (frame != _sel_primary) return; // cbba
        wr_le_u16(b + 0xCu, (uint16_t)(frame - 2u)); // cbc0
        if (sub & 0xF0u) { // cbc8
            sub -= 0x10u; // cbe8
            b[0xBu] = sub;
            if (sub & 0x10u) object_driver_call(0xBu); // cbf0..cbf9
            return;
        }
    }
    _sel_primary = (uint16_t)(_sel_primary - 0xCu); // cbca
    wr_le_u16(b + 0xCu, _sel_primary); // cbd2
    b[0xAu] = 0; // cbd9
    b[0xBu] = 0; // the word store at BP-2 clears both bytes
    ep->handler = _sel_secondary; // cbe2/cbe5: [0x4238]
}

uint8_t entity_frame_info(uint32_t a, uint32_t *src, uint32_t *rows,
                          uint32_t *yoff)
{
    if (!_ent_bank || 4u * a + 4u > _ent_bank_size ||
        2u * a + 0x152u > _ent_bank_size)
        return 0;
    *yoff = rd_be_u16(_ent_bank + 2u * a + 0xE0u);
    *rows = rd_be_u16(_ent_bank + 2u * a + 0x150u);
    *src = rd_be_u16(_ent_bank + 4u * a + 2u) + 0x1C0u;
    return (*src <= _ent_bank_size) ? 1 : 0;
}

uint8_t entity_sprite_pixel(uint32_t src, uint32_t rows, uint32_t x, uint32_t y)
{
    uint32_t idx = src + y * 4u + (x >> 2) + (rows & 0x1Fu) * 8u * (x & 3u);

    if (!_ent_bank || idx >= _ent_bank_size) return 0;
    return _ent_bank[idx] != 0u;
}

Entity *entity_at_mut(uint32_t i)
{ return i < _ent_count ? (Entity *)_ents[i] : NULL; }

void entity_publish_bank(const uint8_t *bank, uint32_t size)
{
    _ent_bank = bank;
    _ent_bank_size = size;
}

void entity_render(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    if (!page || !_ent_bank) return;
    for (uint16_t i = 0; i < _ent_count; i++) {
        const Entity *e = (const Entity *)_ents[i];
        int32_t x = (int32_t)(int16_t)e->x - 0xC;
        int32_t y = (int32_t)(int16_t)e->y;

        if ((int16_t)e->waypoint < 0) continue; // 1000:583D OR CX,CX
        for (uint16_t k = 0; k < 2u; k++) { // 1000:589A + 0x58A4
            uint16_t a = u16(2u * e->waypoint) + k;
            uint32_t src, rows, yoff;

            if (!entity_frame_info(a, &src, &rows, &yoff)) break;
            if (src + rows * 8u > _ent_bank_size) break;
            object_blit_sprite(page, _ent_bank + src, rows, 32u, x - scrollX,
                               y + (int32_t)yoff - scrollY);
            x += 0x20; // 1000:58A1 ADD AX,0x20
        }
    }
}

void entity_tick(void)
{
    /* 1000:C944: AX = [0x64C2]-1, DEC/JNS - the entity table is walked
     * DOWNWARD, so the highest index runs first. */
    for (uint16_t i = _ent_count; i > 0u;) {
        i--;
        Entity *ep = (Entity *)_ents[i];
        /* c94c-c951: BX = +0, CX = +2, DX = +4 into the handler switch.
         * The handlers re-latch their fixed words EVERY call (BP is not
         * carried across ticks), so each handler sees a constant
         * compare/primary/step triple: handler 0 steps +1 px toward x1,
         * handler 1 steps -1 px toward x0. */
        switch (ep->handler) {
        case 0u: // 1000:C96C: DX = CX
            _entity_tail(ep, 2u, 0u, ep->x1);
            break;
        case 1u: // 1000:C973: DX = BX
            _entity_tail(ep, 3u, 2u, ep->x0);
            break;
        case 2u: // 1000:C97A: base 1, target 0x1A
            _sel_secondary = 1u;
            _sel_primary = 0x1Au;
            _entity_carry_anim(ep);
            break;
        case 3u: // 1000:C989: base 0, target 0xC
            _sel_secondary = 0u;
            _sel_primary = 0x0Cu;
            _entity_carry_anim(ep);
            break;
        case 4u: // 1000:C998: the hold ran out
            ep->hold = (uint16_t)(ep->hold - 1u);
            if ((int16_t)ep->hold < 0) { // C99D/C9A1
                ep->waypoint++; // C9A6: INC word [record+0xC]
                ep->handler = 5u; // C9AD
                object_driver_call(0x0Cu); // C9B2
                score_add_level_term(0x7D0u); // C9B9
            }
            break;
        default:
            /* 1000:C9C0: handler 5 is a bare RET. */
            break;
        }
    }
}

int32_t entity_init(const OSystem *osys)
{
    (void)osys;
    return 0;
}

const Entity *entity_at(uint32_t i)
{
    if (i >= _ent_count) return NULL;
    return (const Entity *)_ents[i];
}

uint16_t entity_count(void) { return _ent_count; }

void entity_save_fixed(uint8_t *dst)
{
    if (!dst) return;
    wr_le_u16(dst, _ent_count);
    for (uint16_t i = 0; i < 7u; i++) memcpy(dst + 2u + i * 16u, _ents[i], 16u);
}

void entity_load_fixed(const uint8_t *src)
{
    if (!src) return;
    _ent_count = rd_le_u16(src);
    if (_ent_count > ENT_MAX) _ent_count = ENT_MAX;
    for (uint16_t i = 0; i < 7u; i++) memcpy(_ents[i], src + 2u + i * 16u, 16u);
}
