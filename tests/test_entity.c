/* test_entity.c — the entity system against the shipped data.
 *
 * What is asserted:
 *   1. lb#1 (level 10): exactly one spawn (the single 0xB3 tile at
 *      col 20,row 15) with the closed appender fields: +0 = +6 = 320,
 *      +8 = 193 (row*16 - 0x30 + 1), +0xA = 3, +0xE = 1 (the goal word
 *      0x0101's high byte).  These are the values the DOSBox live
 *      session read off the running game (re_formats.md §12.6, the
 *      live-confirmed tables).
 *   2. the 0xB4 tile at col 25 of the same row joins the walker:
 *      +2 = 400 — the far endpoint, so the dino paces 320 <-> 400.
 *   3. the per-tick machine: the handler re-latches its fixed triple
 *      every call (the baked image words), so the walk is a constant
 *      ±1 px per tick; the flip fires ONLY at an exact endpoint hit;
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
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;

static int bad;

static void failf(const char *what, unsigned id, long got, long want)
{
    fprintf(stderr, "FAIL level %u: %s = %ld, want %ld\n", id, what,
            got, want);
    bad = 1;
}

/* style_handler_prologue (1000:5662), replicated: style 6 + the cell
 * directly below the match (matchIdx + 0x20; the original's +0x1F runs
 * off its +1-moved cursor), <= 0x90, the 0x1F20 flag bit 0x80>>(v&7)
 * => +3 px.  Row 19 reads past the map copy: no shipped B3/B4 there. */
static const uint8_t style6_flags[32] = {
    0x00, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x07,
    0xE0, 0x00, 0x02, 0x02, 0x00, 0x03, 0xC3, 0x81,
    0x80, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static unsigned prologue_bonus(const LevelData *d, unsigned matchIdx)
{
    if (d->style != 6u) return 0;
    if (matchIdx + 0x20u >= 640u) return 0;
    unsigned v = d->map[matchIdx + 0x20u];
    if (v > 0x90u) return 0;
    if ((style6_flags[(v >> 3) & 31u] >> (7u - (v & 7u))) & 1u) return 1;
    return 0;
}

static unsigned count_tiles(const uint8_t *map, unsigned tile)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 640u; i++)
        if (map[i] == tile) n++;
    return n;
}

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

    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }
    char fix_abs[512];
    if (!compat_realpath(fix_dir, fix_abs)) {
        fprintf(stderr, "FAIL fixture tree %s unreadable\n", fix_dir);
        return 1;
    }
    (void)fix_abs;

    entity_init(&osys_null);
    video_init(&osys_null);
    level_init(&osys_null);

    /* 1+2. lb#1: the exact spawn fields and the 0xB4 join. */
    if (level_load(10u) != 0) {
        fprintf(stderr, "FAIL level 10 load\n");
        return 1;
    }
    const LevelData *d = level_data();
    if (d->entityLife != 1u) failf("goal high byte", 10u, d->entityLife, 1);
    entity_spawnFromMap(level_map(), d->style, d->entityLife);
    if (entity_count() != 1u)
        failf("entity count", 10u, (long)entity_count(), 1);

    const Entity *e = entity_at(0);
    if (e) {
        if (e->x0 != 320u) failf("+0 (endpoint A)", 10u, e->x0, 320);
        if (e->x  != 320u) failf("+6 (spawn x)", 10u, e->x, 320);
        if (e->y  != 193u) failf("+8 (spawn y)", 10u, e->y, 193);
        if (e->hold != 3u) failf("+A (hold seed)", 10u, e->hold, 3);
        if (e->waypoint != 0u)
            failf("+C (waypoint)", 10u, e->waypoint, 0);
        if (e->life != 1u) failf("+E (goal high)", 10u, e->life, 1);
        if (e->handler != 0u)
            failf("+4 (dispatch)", 10u, e->handler, 0);
        /* the 0xB4 join lands before any tick: x1 = 400 */
        if (e->x1 != 400u) failf("+2 (endpoint B)", 10u, e->x1, 400);
    }

    /* 3. the walk: a constant +/-1 px step; the flip only at the exact
     * endpoint.  First leg = 80 ticks up (x reaches 400 on tick 79 zero
     * based), the second = 80 ticks down. */
    {
        unsigned lastX = e->x;
        for (unsigned t = 0; t < 170u; t++) {
            entity_tick();
            const Entity *p = entity_at(0);
            if (p->handler > 1u) {
                fprintf(stderr, "FAIL tick %u: +4 = %u (0/1 only)\n", t,
                        p->handler);
                bad = 1;
                break;
            }
            if (p->x != lastX - 1u && p->x != lastX + 1u) {
                fprintf(stderr, "FAIL tick %u: x %u -> %u (not a 1px step)\n",
                        t, lastX, p->x);
                bad = 1;
                break;
            }
            if (p->x == 400u && p->handler == 1u && t != 79u)
                failf("up flip tick", 10u, t, 79);
            if (p->x == 320u && p->handler == 0u && t != 159u)
                failf("down flip tick", 10u, t, 159);
            lastX = p->x;
        }
    }

    /* 4. the whole corpus: count == #B3, the spawn fields (the prologue
     * included), and a 4-tick walk inside each record's window. */
    for (unsigned id = 0; id < 160u; id++) {
        if (level_load(id) != 0) {
            fprintf(stderr, "FAIL level %u: load\n", id);
            return 1;
        }
        const LevelData *lv = level_data();
        entity_spawnFromMap(level_map(), lv->style, lv->entityLife);

        unsigned want = count_tiles(lv->map, 0xB3u);
        if (entity_count() != want)
            failf("count", id, (long)entity_count(), want);

        /* per entity, in spawn (row-major) order: x0 == x == the tile's
         * col*16, y = r*16 (+ the prologue) - 0x30 + 1, and the hold/life
         * seeds.  x1 = the window row's last-scanned 0xB4 (the finder
         * fires per B4 cell; the row-menu scan leaves the max column). */
        unsigned k = 0;
        for (unsigned r = 0; r < 20u; r++) {
            unsigned rowFirst = k;
            for (unsigned c = 0; c < 32u; c++) {
                if (lv->map[r * 32u + c] != 0xB3u) continue;
                const Entity *pe = entity_at(k);
                if (!pe) { failf("missing record", id, k, want); break; }
                if (pe->x0 != c * 16u) failf("x0", id, pe->x0, c * 16u);
                if (pe->x != pe->x0) failf("x == x0", id, pe->x, pe->x0);
                {
                    /* the prologue reads the cell below (matchIdx+0x20)
                     * and grants +3 px; y is a 16-bit word — rows 0..2
                     * wrap negative */
                    unsigned bonus = prologue_bonus(lv, r * 32u + c);
                    uint16_t yWant =
                        (uint16_t)(r * 16u + 3u * bonus - 0x30u + 1u);
                    if (pe->y != yWant) failf("y", id, pe->y, yWant);
                }
                if (pe->hold != 3u) failf("hold", id, pe->hold, 3);
                if (pe->life != lv->entityLife)
                    failf("life", id, pe->life, lv->entityLife);
                k++;
            }
            /* x1 joins: the B4 cells of this row (scan order: max col
             * wins), applied to the FIRST entity appended on the row
             * (the finder's first y match) */
            if (rowFirst < k) {
                unsigned b4c = 32u;
                for (unsigned c = 0; c < 32u; c++)
                    if (lv->map[r * 32u + c] == 0xB4u) b4c = c;
                if (b4c < 32u) {
                    const Entity *pe = entity_at(rowFirst);
                    /* the finder matches the entity's own y against the
                     * B4 cell's computed row word (prologue included);
                     * a prologue mismatch leaves the join unset in the
                     * original too */
                    unsigned bonus = prologue_bonus(lv, r * 32u + b4c);
                    uint16_t yWant =
                        (uint16_t)(r * 16u + 3u * bonus - 0x30u + 1u);
                    if (pe->y == yWant && pe->x1 != b4c * 16u)
                        failf("x1 (B4 join)", id, pe->x1, b4c * 16u);
                }
            }
        }

        /* a 4-tick walk: every record steps -/+1 px per tick from its
         * x0 toward x1 (handler 0 = +1 toward x1; with a 0/400-joined
         * window it stays inside).  No reachable handler != 0/1. */
        for (unsigned t = 0; t < 4u; t++) {
            entity_tick();
            for (unsigned i = 0; i < entity_count(); i++) {
                const Entity *pe = entity_at(i);
                if (pe->handler > 1u) {
                    fprintf(stderr, "FAIL level %u: +4 = %u\n", id,
                            pe->handler);
                    bad = 1;
                }
                /* handler 0 steps +1; x1 = 0 windows walk up freely (the
                 * un-joined walker — the original does the same) */
                if (pe->x < pe->x0 || pe->x > pe->x0 + 4u) {
                    fprintf(stderr, "FAIL level %u: x %u out of window\n",
                            id, pe->x);
                    bad = 1;
                }
            }
        }
    }

    /* 5. la#1 (level 0, no 0xB3): the count stays 0. */
    if (level_load(0u) != 0) {
        fprintf(stderr, "FAIL level 0 load\n");
        return 1;
    }
    entity_spawnFromMap(level_map(), level_data()->style,
                        level_data()->entityLife);
    if (entity_count() != 0u)
        failf("la#1 count", 0u, (long)entity_count(), 0);

    if (!bad)
        printf("entity tests: OK (the lb#1 spawn fields, the 0xB4 join, "
               "the exact-flip walk, the 160-level census)\n");
    return bad;
}
