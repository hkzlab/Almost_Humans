/* entity.c — the entity table and its per-tick dispatch (game_structure.md
 * subsystem 10, re_formats.md §12.6 — the closed system).  The actor is a
 * dinosaur: map tile 0xB3 spawns one, a 0xB4 on the same row supplies the
 * far endpoint, and the game itself alternates the dispatched handler so
 * the dino paces between the two endpoints (the live-verified ping-pong).
 *
 * Ported routines, all read from the Ghidra listing (byte-checked against
 * the listing passages in re_formats.md §12.6):
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
 *   waypoint, which snaps back to the primary when it reaches primary+8 —
 *   an 8-frequency animation clock.  Nothing here reads the map or the
 *   video; the sprite draw is the REX.DAT consumer (not ported yet).
 *
 * Not ported from the tail: the ca43..cb77 body that walks the placed-
 * object array (0x3E75, stride 0x12), tests the ±x/±y overlap windows and
 * posts a type-0x22 record with a CALL 83b7(9) — it reads the entity's x/y
 * and writes nothing of the movement state, so the stub below is exact for
 * the entity record itself; the interaction lands with object.c.
 */
#include "game.h"
#include <stddef.h>

/* --- the record and the array space ------------------------------------ */

#define ENT_MAX  12u
#define MAP_COLS 32u
#define MAP_ROWS 20u

/* The DS globals 0x64C4.. (the exe image at file 0x106F0 + 0x64C4,
 * byte-verified 2026-10-23).  Slots 2 and 3 are not just array slots:
 * handlers 0 and 1 read them as their fixed word tables, and
 * level_init (1000:4CAB) zeroes ONLY the count [0x64C2], so the image's
 * values survive to runtime — this is how a level with up to two 0xB3
 * tiles walks on tables it never wrote (live-verified: the injected single
 * record walked on the baked words; re_plan.md live table +0x4 = 0/1). */
static uint16_t ents[ENT_MAX][8] = {
    /* 0x64C4: slot 0 — the appender's first record (image: zero)       */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* 0x64D4: slot 1 — the appender's second record (image: zero)      */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* 0x64E4: slot 2 — handler 0's fixed words (compare, primary, step,
     * then the walk tail the pair reader walks past)                   */
    { 0x000C, 0x0000, 0x0001, 0xFFDD, 0x0020, 0x000A, 0x0002, 0x0025 },
    /* 0x64F4: slot 3 — handler 1's fixed words start at w[2]           */
    { 0x0003, 0x0008, 0x001A, 0x000E, 0xFFFF, 0x0010, 0x001F, 0x0018 },
    /* 0x6504: slot 4 — the tail of the baked words; appender slot 4
     * starts here (the image is zero from 0x650C)                      */
    { 0x0000, 0xFFF0, 0x0002, 0x0016, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* 0x6514 .. 0x6534: image-zero padding to the end of the reservation */
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
};
static unsigned ent_count;    /* [0x64C2]                                */

/* The dispatcher's shared DS words: [0x4238] = the secondary selector
 * (the movement step this tick), [0x423C] = the primary selector. */
static uint16_t sel_secondary;  /* [0x4238]                              */
static uint16_t sel_primary;    /* [0x423C]                              */

static unsigned cur_style;      /* [0x17EE]: the level header's style    */

/* --- load_terrain_b's data (the exe image, byte-verified) --------------- */

/* 202F:0x1EB1 = the pair count (25); 0x1EB3 = the tile-id bytes; 0x1ECC =
 * the handler offsets.  The port runs only the two handlers that touch
 * the entity array — table #10 (0xB3 -> the appender) and #11 (0xB4 ->
 * the finder); the other 23 handlers spawn placed objects / write the
 * 0x64A2..0x64BA globals and land with object.c. */
static const uint8_t tile_ids[25] = {
    0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
    0xB3, 0xB4, 0xB5, 0xC0, 0xB6, 0xB7, 0xB8, 0xBA, 0xBB, 0xBE,
    0xBF, 0xC1, 0xF0, 0xF1, 0xF2,
};
#define TILE_HANDLER_APPEND  10u   /* 0xB3 -> entity_append_to_array 5360 */
#define TILE_HANDLER_FIND    11u   /* 0xB4 -> entity_find_by_id 5399      */

/* The 0x1F20 byte flag table (image 0x1F20..0x1F3F), the style-6 spawn
 * test's bit source (1000:567d). */
static const uint8_t style6_flags[32] = {
    0x00, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x07,
    0xE0, 0x00, 0x02, 0x02, 0x00, 0x03, 0xC3, 0x81,
    0x80, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* --- the appender (1000:5360) and the prologue (1000:5662) -------------- */

/* style_handler_prologue (1000:5662): the walked record's style is 6 and
 * the map cell DIRECTLY BELOW the match (matchIdx + 0x20 — the +0x1F the
 * prologue reads is offset from the walker's cursor, which has already
 * stepped +1 past the match) is <= 0x90 and passes the 0x1F20 flag bit
 * 0x80>>(v&7): y gains 3 px.  Everything (the finder too) pre-calls this,
 * before the -0x30+1.  A row-19 match reads past the 640-byte map copy
 * into the original's DS neighbours — not reproducible from the map; the
 * corpus has no B3/B4 on row 19, so the port yields 0 there (the guard
 * keeps the read in bounds). */
static int style6_y_bonus(const uint8_t *map, unsigned mapLen,
                          unsigned matchIdx)
{
    if (cur_style != 6u) return 0;
    if (matchIdx + 0x20u >= mapLen) return 0;
    unsigned v = map[matchIdx + 0x20u];
    if (v > 0x90u) return 0;
    uint8_t flag = style6_flags[(v >> 3) & 31u];
    /* MOV AX,1 / SHL AX,CL with CL = 7-(v&7) — bit 0x80>>(v&7) */
    if ((flag >> (7u - (v & 7u))) & 1u) return 1;
    return 0;
}

/* entity_append_to_array (1000:5360..5398).  Summoned per 0xB3 cell in
 * map scan order. */
static void entity_append(const uint8_t *map, unsigned mapLen,
                          unsigned matchIdx, unsigned cx, unsigned dx,
                          unsigned lifeSeed)
{
    if (style6_y_bonus(map, mapLen, matchIdx)) dx += 3;   /* 1000:5698 */

    if (ent_count >= ENT_MAX) {
        /* The original would write past the DS reservation (corrupting
         * the 0x6514-onwards image bytes); the corpus peak is 9 per
         * level, so the port bound is never hit in shipped data. */
        ent_count = ENT_MAX;
        return;
    }
    uint16_t *e = ents[ent_count++];
    unsigned dxAdj = dx + 1u - 0x30u;                     /* 5373/5376 */
    e[0] = (uint16_t)cx;          /* +0 = the spawn column * 16          */
    e[1] = 0;                     /* +2 = 0 (a 0xB4 writes the far end)  */
    e[2] = 0;                     /* +4 = the dispatch index: seeded 0   */
    e[3] = (uint16_t)cx;          /* +6 = current x = the same word      */
    e[4] = (uint16_t)dxAdj;       /* +8 = the spawn row                  */
    e[5] = 3;                     /* +A = the beat countdown seed        */
    e[6] = 0;                     /* +C = the waypoint                   */
    e[7] = (uint16_t)lifeSeed;    /* +E = [0x17F1] = the goal high byte  */
}

/* entity_find_by_id (1000:5399..53b8).  Summoned per 0xB4 cell: rewrites
 * the FIRST entity whose spawn row (+8) matches this row's x1 endpoint. */
static void entity_find(const uint8_t *map, unsigned mapLen,
                        unsigned matchIdx, unsigned cx, unsigned dx)
{
    if (style6_y_bonus(map, mapLen, matchIdx)) dx += 3;   /* 1000:5698 */
    unsigned match = dx + 1u - 0x30u;                     /* 53a5/53a8 */

    for (unsigned i = 0; i < ent_count; i++) {
        if (ents[i][4] != (uint16_t)match) continue;      /* 53a9 */
        ents[i][1] = (uint16_t)cx;                        /* 53ae */
        return;                                           /* 53b1 */
    }
}

/* --- load_terrain_b (1000:5168): the level's map walk ------------------- */

/* One pass over the whole 32x20 map per table pair, in table order, the
 * handler invoked at each matching cell with CX = col*16, DX = row*16
 * (the SHL 4s at 51bf-51cd).  The scan is column-inner, row-outer
 * ([0x4234] = cols 0x1F..0, [0x4230] = rows 0x13..0). */
int entity_spawnFromMap(const uint8_t *map, unsigned style, unsigned lifeSeed)
{
    if (!map) return -1;

    /* level_init (1000:4CAB): the count and nothing else — the fixed
     * table slots keep their image values (the array's initialiser). */
    ent_count = 0;
    cur_style = style;
    sel_secondary = sel_primary = 0;

    for (unsigned pair = 0; pair < 25u; pair++) {
        uint8_t tile = tile_ids[pair];

        for (unsigned row = 0; row < MAP_ROWS; row++) {
            for (unsigned col = 0; col < MAP_COLS; col++) {
                unsigned idx = row * MAP_COLS + col;
                if (map[idx] != tile) continue;
                unsigned cx = col * 16u;
                unsigned dx = row * 16u;
                switch (pair) {
                case TILE_HANDLER_APPEND:
                    entity_append(map, MAP_COLS * MAP_ROWS, idx, cx, dx,
                                  lifeSeed);
                    break;
                case TILE_HANDLER_FIND:
                    entity_find(map, MAP_COLS * MAP_ROWS, idx, cx, dx);
                    break;
                default:
                    /* the placed-object spawners: with object.c */
                    break;
                }
            }
        }
    }
    return 0;
}

/* --- the waypoint tail (1000:C9C1..CA44) -------------------------------- */

/* One tick of the walk.  `wp` = the handler's fixed triple (compare /
 * primary / step) freshly latched from the baked slots, `endX` = the
 * handler's endpoint word (DX: handler 0 passes x1, handler 1 passes
 * x0).  Everything below is the listing, step for step. */
static void entity_tail(Entity *ep, const uint16_t *wp, uint16_t endX)
{
    uint16_t w0 = wp[0];
    sel_primary = w0;                          /* c9c1/c9c5 */
    /* c9c8: ADD BP,2 — the walk branch reads the NEXT words          */

    if (w0 != ep->waypoint) {                  /* c9cb/c9ce: the walk branch */
        sel_primary = wp[1];                   /* c9dd */
        sel_secondary = wp[2];                 /* c9e4/c9e8 */
        ep->x += sel_secondary;                /* c9ee/c9f1: ADD [DI],AX */
        if (endX == ep->x) {                   /* c9f3: the endpoint hit   */
            sel_primary ^= 0x000Eu;            /* c9f7 */
            ep->waypoint = sel_primary;        /* c9fc/c9ff */
            ep->handler ^= 1u;                 /* ca02/ca05: the ping-pong */
            return;
        }
        /* ca09: the beat path (the object-walk + type-0x22 post lives at
         * ca43..cb77 in the original and reads this x/y without writing
         * the movement state — stubbed for now, see the file head). */
        if ((int16_t)--ep->hold >= 0) return;  /* ca0f/ca13/ca17/ca18 */
        ep->hold = 4;                          /* ca1a */
        ep->waypoint += 1u;                    /* ca20 */
        sel_secondary = (uint16_t)(sel_primary + 8u);   /* ca24/ca27/ca2a */
        if (sel_secondary == ep->waypoint)     /* ca2f/ca32/ca36 */
            ep->waypoint = sel_primary;        /* ca38/ca3b: the 8-snap */
        return;
    }

    /* c9d0: the beat branch (w0 == waypoint): hold down; on expiry the
     * waypoint takes the primary... the pair's +2 word (c9d5 uses [BP] at
     * the +2 position = wp[1]). */
    if ((int16_t)--ep->hold < 0)
        ep->waypoint = wp[1];                  /* c9d5/c9d9 */
}

/* --- the dispatch (1000:C93C) ------------------------------------------- */

void entity_tick(void)
{
    for (unsigned i = 0; i < ent_count; i++) {
        Entity *ep = (Entity *)ents[i];
        /* c94c-c951: BX = +0, CX = +2, DX = +4 into the handler switch.
         * The handlers re-latch their fixed words EVERY call (BP is not
         * carried across ticks), so each handler sees a constant
         * compare/primary/step triple: handler 0 steps +1 px toward x1,
         * handler 1 steps -1 px toward x0. */
        switch (ep->handler) {
        case 0u:                               /* 1000:C96C: DX = CX     */
            entity_tail(ep, ents[2], ep->x1);
            break;
        case 1u:                               /* 1000:C973: DX = BX     */
            entity_tail(ep, ents[3] + 2, ep->x0);
            break;
        default:
            /* Handlers 2/3/4/5: no reachable index exists (re_formats.md
             * §12.6 — every reachable writer of +4 stores 0, 1 or 5; the
             * index-4 writer is unreachable), and the shipped corpus
             * never reaches this branch because the flip only yields
             * 0/1.  The original would call the table's own stubs. */
            break;
        }
    }
}

/* --- the interface ------------------------------------------------------ */

int entity_init(const OSystem *osys) { (void)osys; return 0; }

const Entity *entity_at(unsigned i)
{
    if (i >= ent_count) return NULL;
    return (const Entity *)ents[i];
}

unsigned entity_count(void) { return ent_count; }
