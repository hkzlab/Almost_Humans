/* test_entity.c - the entity system against the shipped data.
 *
 * What is asserted:
 *   1. lb#1 (level 10): exactly one spawn (the single 0xB3 tile at
 *      col 20,row 15) with the closed appender fields: +0 = +6 = 320,
 *      +8 = 193 (row*16 - 0x30 + 1), +0xA = 3, +0xE = 1 (the goal word
 *      0x0101's high byte).  These are the values a live session read
 *      off the running game (the live-confirmed tables).
 *   2. the 0xB4 tile at col 25 of the same row joins the walker:
 *      +2 = 400 - the far endpoint, so the dino paces 320 <-> 400.
 *   3. the per-tick machine: the handler re-latches its fixed triple
 *      every call (the baked image words), so the walk is a constant
 *      +/-1 px per tick; the flip fires ONLY at an exact endpoint hit;
 *      the index only ever takes 0 and 1; legs run 80 ticks exactly
 *      (80 px at 1 px/tick), matching the live 21-flips/25 s census.
 *   4. all 160 levels: the spawn count always equals the number of 0xB3
 *      tiles, each record's spawn fields match its tile (the style-6
 *      +3 prologue included), and a short walk keeps every record in
 *      its own endpoint window.
 *   5. la#1 (level 0, no 0xB3): the count stays 0.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root.
 * Missing volumes = skip.
 */
#include "entity.h"
#include "level.h"
#include "object.h"
#include "osystem.h"
#include "res.h"
#include "palette.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"
#include "byteorder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const OSystem g_osys_null;

static int32_t _bad;

/* style_handler_prologue (1000:5662), replicated: style 6 + the cell
 * directly below the match (matchIdx + 0x20; the original's +0x1F runs
 * off its +1-moved cursor), <= 0x90, the 0x1F20 flag bit 0x80>>(v&7)
 * => +3 px.  Row 19 reads past the map copy: no shipped B3/B4 there. */
static const uint8_t _style6_flags[32] = {
    0x00, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x07, 0xE0, 0x00, 0x02,
    0x02, 0x00, 0x03, 0xC3, 0x81, 0x80, 0x01, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/**
 * @brief Print one failure and count it.
 *
 * @param what the check description
 * @param id the identifier
 * @param got the observed value
 * @param want the wanted value
 */
static void _failf(const char *what, uint32_t id, int64_t got, int64_t want)
{
    fprintf(stderr,
            "FAIL level %" PRIu32 ": %s = %" PRId64 ", want %" PRId64 "\n", id,
            what, got, want);
    _bad = 1;
}
/**
 * @brief The style-6 spawn prologue's bonus test for one matched cell.
 *
 * @param d the level record
 * @param matchIdx the matched map index
 * @return 1 when the cell carries the bonus, 0 otherwise
 */
static uint32_t _prologue_bonus(const LevelData *d, uint32_t matchIdx)
{
    if (d->style != 6u) return 0;
    if (matchIdx + 0x20u >= 640u) return 0;
    uint32_t v = d->map[matchIdx + 0x20u];
    if (v > 0x90u) return 0;
    if ((_style6_flags[(v >> 3) & 31u] >> (7u - (v & 7u))) & 1u) return 1;
    return 0;
}

/**
 * @brief Count the cells equal to one tile byte in the 640-cell map.
 *
 * @param map the 640-cell map
 * @param tile the tile byte
 * @return the number of matching cells
 */
static uint32_t _count_tiles(const uint8_t *map, uint32_t tile)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 640u; i++)
        if (map[i] == tile) n++;
    return n;
}

/**
 * @brief Run the entity table tests.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.001", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);

    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }
    char fix_abs[512];
    if (!compat_realpath(fix_dir, fix_abs)) {
        fprintf(stderr, "FAIL fixture tree %s unreadable\n", fix_dir);
        return 1;
    }
    (void)fix_abs;

    res_init(&g_osys_null);
    entity_init(&g_osys_null);
    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    level_init(&g_osys_null);

    /* 1+2. lb#1: the exact spawn fields and the 0xB4 join. */
    if (level_load(10u) != 0) {
        fprintf(stderr, "FAIL level 10 load\n");
        return 1;
    }
    const LevelData *d = level_data();
    if (d->entityLife != 1u) _failf("goal high byte", 10u, d->entityLife, 1);
    entity_spawn_from_map(level_map(), d->style, d->entityLife);
    if (entity_count() != 1u)
        _failf("entity count", 10u, (int64_t)entity_count(), 1);

    const Entity *e = entity_at(0);
    if (e) {
        if (e->x0 != 320u) _failf("+0 (endpoint A)", 10u, e->x0, 320);
        if (e->x != 320u) _failf("+6 (spawn x)", 10u, e->x, 320);
        if (e->y != 193u) _failf("+8 (spawn y)", 10u, e->y, 193);
        if (e->hold != 3u) _failf("+A (hold seed)", 10u, e->hold, 3);
        if (e->waypoint != 0u) _failf("+C (waypoint)", 10u, e->waypoint, 0);
        if (e->life != 1u) _failf("+E (goal high)", 10u, e->life, 1);
        if (e->handler != 0u) _failf("+4 (dispatch)", 10u, e->handler, 0);
        /* the 0xB4 join lands before any tick: x1 = 400 */
        if (e->x1 != 400u) _failf("+2 (endpoint B)", 10u, e->x1, 400);
    }

    /* 3. the walk: a constant +/-1 px step; the flip only at the exact
     * endpoint.  First leg = 80 ticks up (x reaches 400 on tick 79 zero
     * based), the second = 80 ticks down. */
    {
        uint32_t lastX = e->x;
        for (uint32_t t = 0; t < 170u; t++) {
            entity_tick();
            const Entity *p = entity_at(0);
            if (p->handler > 1u) {
                fprintf(stderr, "FAIL tick %u: +4 = %u (0/1 only)\n", t,
                        p->handler);
                _bad = 1;
                break;
            }
            if (p->x != lastX - 1u && p->x != lastX + 1u) {
                fprintf(stderr, "FAIL tick %u: x %u -> %u (not a 1px step)\n",
                        t, lastX, p->x);
                _bad = 1;
                break;
            }
            if (p->x == 400u && p->handler == 1u && t != 79u)
                _failf("up flip tick", 10u, t, 79);
            if (p->x == 320u && p->handler == 0u && t != 159u)
                _failf("down flip tick", 10u, t, 159);
            lastX = p->x;
        }
    }

    /* 2b. the entity sprite law (1000:58A5 / 0x5C5F): the three big-endian
     * frame-table reads, against the shipped apl\rex.dat: index 0's block
     * offset is 0 (so the data starts at 0x1C0), with its y offset and row
     * count.  The bank is published by level_load (1000:4F82). */
    {
        uint32_t src = 0, rows = 0, yoff = 0;

        if (entity_frame_info(0u, &src, &rows, &yoff)) {
            if (src != 0x1C0u)
                _failf("frame 0's source (block 0 + 0x1C0)", 10u, src, 0x1C0);
            if (rows < 32u || rows > 48u)
                _failf("frame 0's row count (a sane sprite)", 10u, rows, 39);
            if (yoff > 64u) _failf("frame 0's y offset", 10u, yoff, 25);
            /* the next cell of the same animation is a different block */
            {
                uint32_t s1 = 0, r1 = 0, o1 = 0;

                if (!entity_frame_info(1u, &s1, &r1, &o1) || s1 == src)
                    _failf("frame 1's source differs", 10u, s1, src);
            }
        } else {
            fprintf(stderr, "FAIL the rex.dat frame tables did not read\n");
            _bad = 1;
        }
    }

    /* 2c. the pass itself, on a synthetic bank: two 0x20-px cells, mode 2
     * (32 px, 8 bytes per row per plane pass), each with its own y offset,
     * row count and source. */
    {
        static uint8_t bank[0x1000];
        static uint8_t page[VIDEO_PAGE_BYTES];
        uint8_t *e8;

        memset(bank, 0, sizeof bank);
        bank[4 * 0 + 3] = 0x00;                 // cell 0: block offset 0
        bank[0xE0] = 0;
        bank[0xE1] = 5;         // its y offset
        bank[0x150] = 0;
        bank[0x151] = 2;       // its row count
        bank[4 * 1 + 3] = 0x40; // cell 1: block offset 0x40
        bank[0xE2] = 0;
        bank[0xE3] = 6;
        bank[0x152] = 0;
        bank[0x153] = 2;
        for (uint32_t i = 0; i < 0x40; i++) { // the two frames' bytes
            bank[0x1C0 + i] = (uint8_t)(0x10u + i);
            bank[0x200 + i] = (uint8_t)(0x80u + i);
        }
        entity_publish_bank(bank, sizeof bank);
        e8 = (uint8_t *)entity_at(0u); // mutate the live record
        if (e8) {
            e8[6] = 100;
            e8[7] = 0; // x = 100
            e8[8] = 50;
            e8[9] = 0; // y = 50
            e8[0xC] = 0;
            e8[0xD] = 0; // waypoint 0
            memset(page, 0, sizeof page);
            entity_render(page, 0, 0);
            /* cell 0 at (100-12, 50+5) = (88, 55), block = 2 rows * 8 */
            if (page[55 * 320 + 88] != bank[0x1C0])
                _failf("cell 0's first pixel", 10u, page[55 * 320 + 88],
                       bank[0x1C0]);
            /* col 31, row 1: block 3 of 4 -> 3*16 + 8 + 7 = 63 */
            if (page[(55 + 1) * 320 + 88 + 31] != bank[0x1C0 + 63])
                _failf("cell 0's last pixel (block = rows*8)", 10u,
                       page[(55 + 1) * 320 + 88 + 31], bank[0x1C0 + 63]);
            /* cell 1 at x + 0x20, y + 6, from block 0x40 */
            if (page[(50 + 6) * 320 + 88 + 0x20] != bank[0x200])
                _failf("cell 1's first pixel", 10u, page[(50 + 6) * 320 + 120],
                       bank[0x200]);
        }
    }

    /* 3b. the tail's interaction pass (1000:CA43): a placed record inside the
     * window is picked up - it becomes type 0x22, carries the entity link at
     * +0xA, and the entity switches to handler 3 (the 0x655C table's
     * entity_select_frame_0c), which the earlier "0/1 only" reading missed. */
    {
        const Entity *p = entity_at(0);
        uint8_t *rec = object_slot(0u);

        if (p && rec && p->handler == 0u) {
            uint32_t ex = p->x, ey = p->y;

            memset(rec, 0, 0x12u);
            wr_le_u16(rec + 0, (uint16_t)(ex + 36u));
            wr_le_u16(rec + 2, (uint16_t)(ey + 30u));
            rec[4] = 0x10u;
            rec[8] = 0x01u; // type 1: pickable (0x6EC3 = 0)
            entity_tick();
            if ((rd_le_u16(rec + 8)) != 0x22u)
                _failf("the pick-up's type", 10u, rec[8], 0x22);
            /* D-59: the +0xA link is the entity, tagged (0x8000 | index) */
            if ((rd_le_u16(rec + 10)) != 0x8000u)
                _failf("the entity link at +0xA", 10u, rec[10], 0x8000);
            if (entity_at(0)->handler != 3u)
                _failf("the carry handler", 10u, entity_at(0)->handler, 3);
            if (entity_at(0)->hold != 0xA032u)
                _failf("the carry hold", 10u, entity_at(0)->hold, 0xA032);
            if (entity_at(0)->waypoint != 8u)
                _failf("the carry waypoint (wp[9])", 10u,
                       entity_at(0)->waypoint, 8);
            /* 1000:CB99: the carry animation runs the hold/sub-step/frame
             * counter and hands the entity back to its walk handler (3's base
             * is 0). */
            {
                uint32_t t = 0;

                while (entity_at(0)->handler == 3u && t++ < 4000u)
                    entity_tick();
                if (entity_at(0)->handler != 0u)
                    _failf("the carry animation returns to handler 0", 10u,
                           entity_at(0)->handler, 0);
                if (entity_at(0)->hold != 0u)
                    _failf("the carry animation clears the hold", 10u,
                           entity_at(0)->hold, 0);
            }
        }
    }

    /* 3b2. D-96: the guard reversal (1000:CABC -> 1000:CB78).  A placed record
     * whose type is the handler's wp[4] (0x20 for handler 0, 0x1F for handler
     * 1) is the spear/torch guard stance op 5 writes; the original flips the
     * entity's waypoint and dispatch index instead of carrying it. */
    {
        Entity *p = entity_at_mut(0u);
        uint8_t *rec = object_slot(0u);

        if (p && rec) {
            /* handler 0, walking right: wp[3] = -35, so rec.x = x + 36 */
            p->handler = 0u;
            p->waypoint = 0u;
            p->hold = 0u;
            p->x = 360u;
            memset(rec, 0, 0x12u);
            wr_le_u16(rec + 0, (uint16_t)(p->x + 36u));
            wr_le_u16(rec + 2, (uint16_t)(p->y + 30u));
            rec[4] = 0x10u;
            rec[8] = 0x20u; // handler 0's wp[4]: a guard facing left
            entity_tick();
            if (p->handler != 1u)
                _failf("D-96: the guard reverses handler 0", 10u, p->handler,
                       1);
            if (p->waypoint != 0x0Eu)
                _failf("D-96: the guard sets the waypoint", 10u, p->waypoint,
                       0x0E);
            if ((uint32_t)(rd_le_u16(rec + 8)) != 0x20u)
                _failf("D-96: the guard record is not carried", 10u,
                       rd_le_u16(rec + 8), 0x20);
            if ((uint32_t)(rd_le_u16(rec + 0xAu)) != 0u)
                _failf("D-96: the guard record has no entity link", 10u,
                       rd_le_u16(rec + 0xAu), 0);

            /* handler 1, walking left: wp[3] = +16, so rec.x = x - 15 */
            p->handler = 1u;
            p->waypoint = 0x0Eu;
            p->hold = 4u;
            p->x = 360u;
            memset(rec, 0, 0x12u);
            wr_le_u16(rec + 0, (uint16_t)(p->x - 15u));
            wr_le_u16(rec + 2, (uint16_t)(p->y + 30u));
            rec[4] = 0x10u;
            rec[8] = 0x1Fu; // handler 1's wp[4]: a guard facing right
            entity_tick();
            if (p->handler != 0u)
                _failf("D-96: the guard reverses handler 1", 10u, p->handler,
                       0);
            if (p->waypoint != 0u)
                _failf("D-96: the guard restores the waypoint", 10u,
                       p->waypoint, 0);
            if ((uint32_t)(rd_le_u16(rec + 8)) != 0x1Fu)
                _failf("D-96: the guard record stays type 0x1F", 10u,
                       rd_le_u16(rec + 8), 0x1F);
        }
    }

    /* 3c. D-59: the full eaten chain.  The carry link points at the entity,
     * so the type-34 tail marks the human consumed while the entity is on
     * handlers 2/3 and runs 0xF5BC when the carry ends; the probe's F682 arm
     * turns the record into the type-0x21 lost marker and rolls the bar's
     * tribe counter down. */
    {
        uint8_t *cr;
        uint8_t params[8];
        uint32_t t, type;

        (void)level_load(10u);
        entity_spawn_from_map(level_map(), level_data()->style,
                              level_data()->entityLife);
        object_init();
        cr = object_slot(0u);
        {
            const Entity *p = entity_at(0u);
            uint32_t px = p ? p->x : 320u, py = p ? p->y : 193u;

            memset(cr, 0, 0x12u);
            wr_le_u16(cr + 0, (uint16_t)(px + 36u));
            wr_le_u16(cr + 2, (uint16_t)(py + 30u));
            cr[4] = 0x10u;
            cr[8] = 0x01u;
        }
        object_set_action_entity(0u);
        entity_tick();
        if ((rd_le_u16(cr + 8)) != 0x22u)
            _failf("D-59: the pick-up", 10u, cr[8], 0x22);
        statusbar_set_tribe_digits(1u, 2u); // the bar shows 12
        for (t = 0; t < 4000u; t++) {
            type = (uint32_t)(rd_le_u16(cr + 8));
            if (type == 0x21u) break;
            entity_tick(); // the game's order: world first
            (void)object_command_run(cr, 0u);
        }
        type = (uint32_t)(rd_le_u16(cr + 8));
        if (type != 0x21u)
            _failf("D-59: the carried human is consumed", 10u, type, 0x21);
        statusbar_save_params(params);
        if (params[0] != 0xFFu)
            _failf("D-59: the loss arms the tribe counter", 10u, params[0],
                   0xFF);
        if ((rd_le_u16(cr + 4)) != 0x2Du)
            _failf("D-59: the lost marker's frame", 10u, cr[4], 0x2D);
    }

    /* 3d. D-60: the play loop's exit stops update_world (1000:34E6), so the
     * world behind the end caption is frozen even though level_tick is still
     * called for the redraw. */
    {
        uint32_t x0, x1;

        (void)level_load(10u);
        entity_spawn_from_map(level_map(), level_data()->style,
                              level_data()->entityLife);
        level_begin();
        for (uint32_t t = 0; t < 3000u && level_starting(); t++) {
            if (level_loading_screen()) level_loading_tick();
            else level_tick();
            palette_tick();
        }
        if (level_starting()) {
            fprintf(stderr, "FAIL the level never reached play\n");
            _bad = 1;
        } else {
            x0 = entity_at(0u)->x;
            level_tick();
            palette_tick();
            x1 = entity_at(0u)->x;
            if (x1 == x0) _failf("the playing world moves", 10u, x1, x0 + 1);

            level_set_end(0); // the pause's RESTART THIS GAME
            x0 = entity_at(0u)->x;
            for (uint32_t t = 0; t < 10u; t++) {
                level_tick(); // the caption's per-tick call
                palette_tick();
            }
            x1 = entity_at(0u)->x;
            if (x1 != x0)
                _failf("the end caption freezes the world", 10u, x1, x0);
        }
    }

    /* 4. the whole corpus: count == #B3, the spawn fields (the prologue
     * included), and a 4-tick walk inside each record's window. */
    for (uint32_t id = 0; id < 160u; id++) {
        if (level_load(id) != 0) {
            fprintf(stderr, "FAIL level %u: load\n", id);
            return 1;
        }
        const LevelData *lv = level_data();
        entity_spawn_from_map(level_map(), lv->style, lv->entityLife);

        uint32_t want = _count_tiles(lv->map, 0xB3u);
        if (entity_count() != want)
            _failf("count", id, (int64_t)entity_count(), want);

        /* per entity, in spawn (row-major) order: x0 == x == the tile's
         * col*16, y = r*16 (+ the prologue) - 0x30 + 1, and the hold/life
         * seeds.  x1 = the window row's last-scanned 0xB4 (the finder
         * fires per B4 cell; the row-menu scan leaves the max column). */
        uint32_t k = 0;
        for (uint32_t r = 0; r < 20u; r++) {
            uint32_t rowFirst = k;
            for (uint32_t c = 0; c < 32u; c++) {
                if (lv->map[r * 32u + c] != 0xB3u) continue;
                const Entity *pe = entity_at(k);
                if (!pe) {
                    _failf("missing record", id, k, want);
                    break;
                }
                if (pe->x0 != c * 16u) _failf("x0", id, pe->x0, c * 16u);
                if (pe->x != pe->x0) _failf("x == x0", id, pe->x, pe->x0);
                {
                    /* the prologue reads the cell below (matchIdx+0x20)
                     * and grants +3 px; y is a 16-bit word - rows 0..2
                     * wrap negative */
                    uint32_t bonus = _prologue_bonus(lv, r * 32u + c);
                    uint16_t yWant =
                        (uint16_t)(r * 16u + 3u * bonus - 0x30u + 1u);
                    if (pe->y != yWant) _failf("y", id, pe->y, yWant);
                }
                if (pe->hold != 3u) _failf("hold", id, pe->hold, 3);
                if (pe->life != lv->entityLife)
                    _failf("life", id, pe->life, lv->entityLife);
                k++;
            }
            /* x1 joins: the B4 cells of this row (scan order: max col
             * wins), applied to the FIRST entity appended on the row
             * (the finder's first y match) */
            if (rowFirst < k) {
                uint32_t b4c = 32u;
                for (uint32_t c = 0; c < 32u; c++)
                    if (lv->map[r * 32u + c] == 0xB4u) b4c = c;
                if (b4c < 32u) {
                    const Entity *pe = entity_at(rowFirst);
                    /* the finder matches the entity's own y against the
                     * B4 cell's computed row word (prologue included);
                     * a prologue mismatch leaves the join unset in the
                     * original too */
                    uint32_t bonus = _prologue_bonus(lv, r * 32u + b4c);
                    uint16_t yWant =
                        (uint16_t)(r * 16u + 3u * bonus - 0x30u + 1u);
                    if (pe->y == yWant && pe->x1 != b4c * 16u)
                        _failf("x1 (B4 join)", id, pe->x1, b4c * 16u);
                }
            }
        }

        /* a 4-tick walk: every record steps -/+1 px per tick from its
         * x0 toward x1 (handler 0 = +1 toward x1; with a 0/400-joined
         * window it stays inside).  No reachable handler != 0/1. */
        for (uint32_t t = 0; t < 4u; t++) {
            entity_tick();
            for (uint32_t i = 0; i < entity_count(); i++) {
                const Entity *pe = entity_at(i);
                if (pe->handler > 1u) {
                    fprintf(stderr, "FAIL level %u: +4 = %u\n", id,
                            pe->handler);
                    _bad = 1;
                }
                /* handler 0 steps +1; x1 = 0 windows walk up freely (the
                 * un-joined walker - the original does the same) */
                if (pe->x < pe->x0 || pe->x > pe->x0 + 4u) {
                    fprintf(stderr, "FAIL level %u: x %u out of window\n", id,
                            pe->x);
                    _bad = 1;
                }
            }
        }
    }

    /* 5. la#1 (level 0, no 0xB3): the count stays 0. */
    if (level_load(0u) != 0) {
        fprintf(stderr, "FAIL level 0 load\n");
        return 1;
    }
    entity_spawn_from_map(level_map(), level_data()->style,
                          level_data()->entityLife);
    if (entity_count() != 0u)
        _failf("la#1 count", 0u, (int64_t)entity_count(), 0);

    /* 6. handler 4 (1000:C998): the mover collision's life word drives it, and
     * the hold counts down every tick; the tick that takes the hold negative
     * bumps the waypoint, switches to the no-op handler 5 and adds 0x7D0 to
     * [0x1DCA]. */
    if (level_load(10u) == 0) {
        Entity *p;

        entity_spawn_from_map(level_map(), level_data()->style,
                              level_data()->entityLife);
        p = entity_at_mut(0u);
        if (p) {
            uint16_t wp = p->waypoint;

            p->handler = 4u;
            p->hold = 9u;
            entity_tick();
            if (p->handler != 4u)
                _failf("handler 4 holds while the hold lasts", 10u, p->handler,
                       4);
            p->hold = 0u; // 1000:C99D's DEC wraps it to 0xFFFF
            entity_tick();
            if (p->handler != 5u)
                _failf("the hold's expiry", 10u, p->handler, 5);
            if (p->waypoint != (uint16_t)(wp + 1u))
                _failf("the expiry bumps the waypoint", 10u, p->waypoint,
                       (int64_t)wp + 1);
            entity_tick();
            if (p->handler != 5u)
                _failf("handler 5 is the bare RET", 10u, p->handler, 5);
        }
    }

    if (!_bad)
        printf("entity tests: OK (the lb#1 spawn fields, the 0xB4 join, "
               "the exact-flip walk, the 160-level census)\n");
    return _bad;
}
