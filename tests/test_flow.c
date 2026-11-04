/* test_flow.c — the flow's connections (entry's state machine + level_run's
 * arms), against the decoded original:
 *
 *   level_run 3060/3062   need > members -> the level is skipped
 *   entry LAB_00CD        [0x22]++/[0x26]-- -> the next level, else COMPLETE
 *   level_run's end       object_test_dispatch < 0 -> [0x138E] selects the
 *                         "WELL DONE" / "EXTINCT" page (draw_level_text)
 *   level_restore 35AF    [0x11FC] := [0x1970] (the level-start members)
 *
 * argv[1] = the game data directory (HUMANS.000/001/002).  A no-data run
 * skips the level-loading parts.
 */
#include "entity.h"
#include "gameflow.h"
#include "hiscore.h"
#include "level.h"
#include "map.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "score.h"
#include "statusbar.h"
#include "textpage.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);
extern VideoMode osys_null_videoMode(void);

extern const OSystem osys_null;

static int32_t bad;

static void flow_dump_ppm(const char *path, const uint8_t *page)
{
    FILE *f = fopen(path, "wb");
    uint32_t i;

    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (i = 0; i < 320u * 200u; i++) {
        uint8_t v = page[i] % 64u;
        fputc(v * 4, f);
        fputc(v * 4, f);
        fputc(v * 4, f);
    }
    fclose(f);
}

static void check(int32_t ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        bad = 1;
    }
}

/* the record-word helpers, also used by the walk/marker blocks below */
static uint32_t tf_rd16(const uint8_t *r, uint32_t o)
{ return (uint32_t)(r[o] | (r[o + 1] << 8)); }
static void tf_wr16(uint8_t *r, uint32_t o, uint32_t v)
{
    r[o] = (uint8_t)v;
    r[o + 1] = (uint8_t)(v >> 8);
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    video_init(&osys_null);
    palette_init(&osys_null);
    level_init(&osys_null);
    statusbar_init(&osys_null);       /* the charge bar draws into the band */
    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }
    {
        char path[512];
        snprintf(path, sizeof path, "%s/HUMANS.001", vol_dir);
        FILE *probe = fopen(path, "rb");
        if (!probe) {
            printf("skip: no volume files at %s\n", vol_dir);
            return 0;
        }
        fclose(probe);
    }
    res_setDisc(1u);

    /* 1. level_run's gate: need > members skips the level */
    check(!gameflow_tribeTooSmall(4u, 12u), "need 4 <= 12 starts");
    check(!gameflow_tribeTooSmall(4u, 4u), "need 4 == 4 starts");
    check(gameflow_tribeTooSmall(4u, 3u), "need 4 > 3 skips");
    check(!gameflow_tribeTooSmall(0u, 0u), "no need always starts");

    /* 2. entry LAB_00CD: [0x22]++ while the 160 levels last, then COMPLETE */
    {
        uint32_t lv = 0u;
        check(gameflow_afterLevel(&lv) == FLOW_LEVEL && lv == 1u,
              "level 0 -> the next level");
        lv = 158u;
        check(gameflow_afterLevel(&lv) == FLOW_LEVEL && lv == 159u,
              "level 158 -> 159");
        lv = 159u;
        check(gameflow_afterLevel(&lv) == FLOW_ATTRACT && lv == 159u,
              "the last level -> COMPLETE (the attract)");
    }

    /* 3. load_sprite_pal's LOADING screen (1000:1DC0): ten walkers for
     * 0xA0 frames, then the letters (blocks 0x11..0x18) — the letters land
     * as the 16x24 "L" block at x=0xB0 falling from y=0x1A. */
    if (level_load(0) == 0) {
        /* level_run 1000:3054 runs level_video_init's two fades (1000:3083)
         * before the loading screen (1000:3086), so drive the chain to it. */
        for (uint32_t t = 0; t < 400u && !level_loadingScreen(); t++) {
            level_begin();
            level_tick();
            palette_tick();
        }
        check(level_loadingScreen(), "the LOADING screen starts");
        /* load_sprite_pal's fade (1000:1E33): 0x80 entries toward
         * dicon+0x18, div 0x19, before the frames */
        check(palette_fadeActive(), "the LOADING screen fades its palette in");
        for (uint32_t t = 0; t < 60u && palette_fadeActive(); t++)
            palette_tick();
        check(!palette_fadeActive(), "the loading fade completes");
        {
            char dp[512];
            snprintf(dp, sizeof dp, "%s/HUMANS.001/BACKS/CICON.APL", fix_dir);
            FILE *df = fopen(dp, "rb");
            if (df) {
                uint8_t buf[0x198];
                size_t n = fread(buf, 1, sizeof buf, df);
                fclose(df);
                if (n == sizeof buf && buf[0] == 'A')
                    check(memcmp(osys_null_palette(), buf + 0x18, 0x180) == 0,
                          "the loading DAC is dicon+0x18");
            }
        }
        /* the background: a clean black page plus BACKS\DICON.APL's 80x56
         * card at (0x50,0x60) above the status bar — the 1000:3310 blit's
         * visible position, 4 px/byte (not doubled) */
        level_loadingTick();               /* the first frame composes it */
        {
            char dp[512];
            snprintf(dp, sizeof dp, "%s/HUMANS.001/BACKS/CICON.APL", fix_dir);
            FILE *df = fopen(dp, "rb");
            if (df) {
                uint8_t dic[0x1580];
                size_t n = fread(dic, 1, sizeof dic, df);
                fclose(df);
                if (n == sizeof dic && dic[0] == 'A') {
                    const uint8_t *page = osys_null_page(0);
                    check(page[0] == 0u, "the loading background is cleared");
                    check(page[(0x60u + 10u) * 320u + 0x78u + 40u] ==
                                  dic[0x400u + 10u * 80u + 40u] &&
                              page[(0x60u + 10u) * 320u + 0x78u + 41u] ==
                                  dic[0x400u + 10u * 80u + 41u],
                          "the style card is at (0x78,0x60), 1x");
                }
            }
        }
        for (uint32_t t = 0; t < 0x9Fu; t++) level_loadingTick();
        check(level_loadingScreen(), "the letters phase follows");
        {
            uint32_t sz = 0;
            char lp[512];
            snprintf(lp, sizeof lp, "%s/HUMANS.001/APL/LOADING.APL", fix_dir);
            FILE *f = fopen(lp, "rb");
            uint8_t *bank = NULL;
            if (f) {
                fseek(f, 0, SEEK_END);
                int64_t n = ftell(f);
                fseek(f, 0, SEEK_SET);
                bank = malloc((size_t)n);
                if (!bank || fread(bank, 1, (size_t)n, f) != (size_t)n) {
                    free(bank);
                    bank = NULL;
                } else sz = (uint32_t)n;
                fclose(f);
            }
            check(bank && sz >= 0x400u + 0x19u * 0x180u,
                  "LOADING.APL readable");
            if (bank) {
                /* the state after one tick: the "L" at (0xB0, 0x1C) — the
                 * template's y=0x1A plus one dy step */
                for (uint32_t k = 0; k < 0x20u; k++) level_loadingTick();
                const uint8_t *page = osys_null_page(0);
                const uint8_t *L = bank + 0x400u + 0x11u * 0x180u;
                uint32_t badpx = 0;
                for (uint32_t r = 0; r < 24u; r++)
                    for (uint32_t c = 0; c < 16u; c++)
                        if (L[r * 16u + c] &&
                            page[(0x24u + r) * 320u + 0x60u + c] !=
                                L[r * 16u + c])
                            badpx++;
                check(badpx == 0u, "the 'L' block drawn at x=0x60 (bad chars)");
                free(bank);
            }
        }
        for (uint32_t t = 0; t < 0x60u; t++) level_loadingTick();
        check(!level_loadingScreen(), "the LOADING screen finishes");
        if (level_load(0) == 0 && !level_loadingScreen()) {
            /* a fresh load starts again with the chain's fades to black */
            level_begin();
            for (uint32_t t = 0; t < 400u && !level_loadingScreen(); t++) {
                level_begin();
                level_tick();
                palette_tick();
            }
            check(level_loadingScreen(), "a reload restarts the screen");
            for (uint32_t t = 0; t < 0x100u; t++) level_loadingTick();
        }
    } else printf("skip: the level data is not on the disc\n");

    /* 4. the level's own end: the stub hook + [0x138E]'s outcome */
    if (level_load(0) == 0) {
        check(!level_ended(), "a fresh level is in play");
        level_requestEnd(1);
        check(level_ended() && level_outcome(), "won: [0x138E] stays 0xFF");
        check(level_load(0) == 0 && !level_ended(),
              "a reload clears the end flag ([0x138E] := 0xFF at 3068)");
        level_requestEnd(0);
        check(level_ended() && !level_outcome(), "extinct: [0x138E] = 0");

        /* 4. the start snapshot [0x1970] and level_restore's reset */
        {
            uint32_t req = level_required(), mem = level_members();
            const uint8_t *map = level_map();
            uint32_t counted = 0;
            for (uint32_t i = 0; i < 640u; i++)
                if (map[i] == 0xAAu) counted++;
            check(req == counted, "required = the map's 0xAA count");
            check(mem == 12u, "the fresh board's 12 members");
            level_restoreMembers();
            check(level_members() == mem && level_required() == req,
                  "level_restore restores the start snapshot");
        }

        /* 5. draw_level_text's two pages render (and differ) */
        {
            static uint8_t base[64000], won[64000], gone[64000];
            memcpy(base, osys_null_page(0), sizeof base);
            memcpy(won, base, sizeof won);
            memcpy(gone, base, sizeof gone);
            textpage_draw(won, "WELL DONE! YOU FINISHED LEVEL 001", -1, 0u, 0);
            textpage_draw(gone, "YOUR TRIBE IS EXTINCT!!", -1, 0u, 0);
            check(memcmp(base, won, sizeof base) != 0, "the end page draws");
            check(memcmp(won, gone, sizeof won) != 0,
                  "the two end texts differ");
        }
    } else {
        printf("skip: the level data is not on the disc\n");
    }

    /* 6. the level-end trigger (object_test_dispatch 1000:35D6): the goal
     * hook wins; losing the whole tribe ends the level with [0x138E]=0 */
    if (level_load(0) == 0) {
        level_goalReached();
        check(level_endTest() && level_ended() && level_outcome(),
              "the goal hook ends the level as won");
        /* check_highscore's score (1000:41AE): [0x1F41]*[0x11FC]*10 (55EA)
         * + the remaining seconds*10 (569B..56CB) */
        {
            statusbar_start(0x0600u, 12u, 4u);        /* 6:00, 12 members */
            check(score_levelScore() == 1u * 12u * 10u + 360u * 10u,
                  "the score: level 1 x 12 members + 6:00 left = 3720");
            statusbar_start(0x0530u, 12u, 4u);        /* 5:30 left */
            check(score_levelScore() == 1u * 12u * 10u + 330u * 10u,
                  "the score follows the remaining time (5:30 = 3420)");
        }
        check(level_load(0) == 0 && !level_ended(),
              "a reload clears the end trigger");
        {
            uint32_t n = level_members();
            for (uint32_t i = 0; i < n; i++) level_memberLost();
            check(level_members() == 0u, "the tribe count reaches zero");
            check(level_endTest() && level_ended() && !level_outcome(),
                  "an extinct tribe ends the level as lost");
            check(score_levelScore() == 0u,
                  "a lost level adds no score (55B3 -> 578A)");
        }
        (void)level_load(0);
    } else printf("skip: the level data is not on the disc\n");

    /* 6b. the placed-object array (0x3E75) + object_test_dispatch's
     * predicates (1000:35D6) and the claim's writes (1000:3A6A) */
    {
        uint8_t *r;
        static uint8_t testPal[768]; /* the level palette the fades return to */
        object_init();
        check(
            object_staticSlots() == 8u && object_slots() == 29u &&
                object_liveCount() == 8u && object_current() == -1,
            "the placed array starts at 8 used (load_terrain_b) with none current");
        check(object_noneType1d() == -1, "no 0x1D record -> true");
        check(object_typeEquals(3u) == 0, "record 0 is not type 3");

        /* the player at (0x100, 0x80); a neighbour 0x20 right, 8 down, type 2 */
        object_setPlayer(0);
        r = object_slot(0);
        r[0] = 0x00;
        r[1] = 0x01;
        r[2] = 0x80;
        r[3] = 0x00;
        r[6] = 0x00; /* active */
        object_setCurrent(0);
        object_setActionEntity(0u);
        check(object_findNeighbour(0) == -1, "an empty array has no neighbour");

        r = object_slot(3);
        r[0] = 0x20;
        r[1] = 0x01;
        r[2] = 0x88;
        r[3] = 0x00; /* +0x20, +8 */
        r[6] = 0x00; /* active    */
        r[8] = 0x02; /* type 2    */
        check(object_findNeighbour(0) == 3, "the window finds the neighbour");
        check(object_typeEquals(2u) == 0,
              "the predicate reads the *acting* record");
        object_setActionEntity(3u);
        check(object_typeEquals(2u) == -1, "the neighbour's type matches");
        check(object_classMatches(0x15u) == 0, "its class is 0");
        object_setActionEntity(0u);

        /* D-175: `[0x3E5E + rec[+7]]` is a byte of the data segment, not the
         * 0x40-byte class array.  0x3E5E + 0x15 = 0x3E73 (the live count),
         * 0x3E5E + 0x99 = 0x3EF7 (record 7's +4). */
        {
            uint8_t *a = object_slot(0), *w = object_slot(7u);

            a[7] = 0x15u;
            check(object_classMatches(object_liveCount() & 0x3Fu) == -1,
                  "class byte 0x15 reads the live count (0x3E73)");
            a[7] = 0x99u;
            w[4] = 0x05u;
            check(object_classMatches(5u) == -1,
                  "class byte 0x99 reads record 7's +4 (0x3EF7)");
            w[4] = 0u;
            a[7] = 0u;
        }

        /* out of the window on either axis, inactive, or type >= 3: no claim */
        r[8] = 0x03;
        check(object_findNeighbour(0) == -1, "a type >= 3 record is skipped");
        r[8] = 0x00;
        r[6] = 0x80; /* inactive  */
        check(object_findNeighbour(0) == -1, "an inactive record is skipped");
        r[6] = 0x00;
        r[0] = 0x22;
        r[1] = 0x01; /* +0x22     */
        check(object_findNeighbour(0) == -1, "0x22 is outside |dx| <= 0x20");
        r[0] = 0x20;
        r[1] = 0x01;
        r[2] = 0x90;
        r[3] = 0x00; /* +0x10     */
        check(object_findNeighbour(0) == -1, "0x10 is outside |dy| <= 8");
        r[2] = 0x88;
        r[3] = 0x00;

        check(object_noneType1d() == -1, "still no 0x1D record");
        r[8] = 0x1d;
        check(object_noneType1d() == 0, "a 0x1D record falsifies it");
        r[8] = 0x02;

        /* the claim (1000:3A6A): the gate, the scan, the writes and then the
         * whole sequence — red/white flash, blinks, the animation, the spawn
         * and the settle — driven with object_tick + palette_tick */

        /* the window is not 4-aligned on the way in: 1000:3B28 snaps it */
        level_setView(18u, 8u);
        check(object_claimBegin(0u, 0) == 3, "the claim takes the neighbour");
        check(r[4] == 0x03 && r[5] == 0x01,
              "the neighbour's frame becomes 0x103");
        check(r[6] == 0 && r[7] == 0, "the neighbour's flags word clears");
        check(object_slot(0)[4] == 0x20 && object_slot(0)[5] == 0,
              "the old current object's frame becomes 0x20");
        check(object_current() == 3, "the claim makes the neighbour current");
        check(object_claimActive() == 1, "the sequence starts");
        uint32_t spawned = 0;
        {
            uint32_t t = 0, moved = 0;
            int32_t frozen = 0;

            /* 1000:3B19/3B28: the sequence freezes the world and snaps the
             * window to 4 px; nothing in it writes the window (the 3D41/3DC0
             * pair is a strip SAVE), so the camera must not move. */
            check(level_viewX() == 16u && level_viewY() == 8u,
                  "the claim snaps the window to 4 px, without moving it");
            while (object_claimActive() && t++ < 20000u) {
                palette_tick();
                object_tick(testPal);
                if (!level_updates()) frozen = 1;
                if (level_viewX() != 16u || level_viewY() != 8u) moved = 1;
            }
            check(frozen, "the claim freezes update_world ([0xC] = 0)");
            check(!moved, "the claim never moves the window");
            check(level_updates(), "the claim restores [0xC] (1000:3CAB)");
            check(!object_claimActive(), "the claim sequence finishes");
            check(level_sparkActive(),
                  "the claim's append starts the particle effect");
            /* the particles run in play: the chain (fades + loading) is driven
             * first, and the effect's spawns are counted from the first play
             * frame (sparkStep is PLAYING-only, like the original's frame body) */
            if (level_starting()) {
                for (uint32_t i = 0; i < 1200u && level_starting(); i++) {
                    level_begin(); /* waits for a running fade */
                    level_tick();
                    palette_tick();
                    if (level_loadingScreen()) level_loadingTick();
                }
            }
            {
                uint32_t t2 = 0, seen = 0, bitOK = 1;

                /* the particles are drawn by the compose, which only runs once
                 * the start chain has finished */
                if (level_starting()) {
                    level_begin();
                    for (uint32_t i = 0; i < 1200u && level_starting(); i++) {
                        level_begin(); /* waits for a running fade */
                        level_tick();
                        palette_tick();
                        if (level_loadingScreen()) level_loadingTick();
                    }
                }
                while (level_sparkActive() && t2++ < 400u) {
                    uint32_t sx = 0, sy = 0, sv = 0;

                    level_tick();
                    if (!seen && level_sparkPeek(&sx, &sy, &sv) && sv != 0u) {
                        uint32_t px = sx - level_viewX();
                        uint32_t py = sy - level_viewY();
                        const uint8_t *pg = osys_null_page(0);

                        if (px < 320u && py < 200u) {
                            seen = 1;
                            bitOK = (((pg[py * 320u + px] >> (px & 3u)) & 1u) ==
                                     ((sv >> (px & 3u)) & 1u));
                        }
                    }
                }
                check(seen != 0u, "a spawned cell carries a sprite value");
                check(bitOK != 0u,
                      "the composed pixel carries the cell's plane bit");
                check(!level_sparkActive(),
                      "the effect ends after its 0x53 life");
            }
            spawned = object_liveCount();
            check(spawned == 9u, "the spawn appended one live record");
            if (spawned == 9u) {
                /* 0x3F17 + 0x12*(8-8) = port index 9, class 8-7 = 1 */
                const uint8_t *nr = object_slot(9);
                check((nr[0] | (nr[1] << 8)) == 0x0120u,
                      "the spawned record takes the neighbour's x");
                check((nr[2] | (nr[3] << 8)) == 0x88u + 9u,
                      "the spawned record sits 9 below the neighbour");
                check((nr[4] | (nr[5] << 8)) == 0x0060u,
                      "its frame is the recipe's 0x60 (the spear)");
                check((nr[8] | (nr[9] << 8)) == 0xFFFFu,
                      "its +8 is the recipe's first parameter");
                check(object_slotClass(1u) == 5u,
                      "its behaviour 5 lands in class slot 1");
            }
            check(r[6] & 0x80, "the settle step removes the neighbour");
            /* the animation's six 4FE9 increments over its 6 x 9 frames */
            check((r[4] | (r[5] << 8)) == 0x0109u,
                  "the neighbour animates 0x103 -> 0x109");
            /* D-48: 1000:34E6 — the play loop's exit.  The tick that detects
             * the end plays sound 0xC (1000:83B7) and freezes the world for
             * the 15 x 1000:8B56 waits before draw_level_text's chain. */
            if (!level_starting()) {
                uint32_t h;

                level_requestEnd(1);
                level_tick();
                check(level_endHolding() && !level_ended(),
                      "1000:34E6: the exit holds the frozen world");
                check(object_lastSfx() == 0x0Cu,
                      "1000:34E6: the exit plays sound 0xC");
                for (h = 0; h < 15u; h++) {
                    check(level_endHolding(), "1000:34E6: 15 waits");
                    level_tick();
                }
                check(!level_endHolding() && level_ended(),
                      "1000:34E6: the caption follows the 15 waits");
            }
            object_init();
        }
    }

    /* 6c. the goal test (object_test_dispatch 1000:35D6) and the spawn
     * recipes (202F:0x19BA) */
    {
        uint8_t *r;
        object_init();
        check(level_data() && level_data()->objectTest == 0x0502u,
              "level 1's goal test word is 0x0502 (mode 2, arg 5)");
        check(object_testDispatch(0x0502u) == 0,
              "an empty array does not meet it");
        object_setCurrent(0);
        object_setSlotClass(1u, 5u);
        object_slot(0)[7] = 1u; /* the record's source slot */
        check(object_testDispatch(0x0502u) == 1, "class 5 in slot 1 meets it");
        object_setSlotClass(1u, 4u);
        check(object_testDispatch(0x0502u) == 0, "class 4 does not");
        object_setSlotClass(1u, 5u);
        /* level_endTest is object_test_dispatch now */
        (void)level_load(0);
        object_setCurrent(0);
        object_setSlotClass(1u, 5u);
        object_slot(0)[7] = 1u;
        check(level_endTest() && level_outcome(),
              "the level's own goal test ends it as won");

        /* 0x36AB: the map cell under the record (the helper is 0xF761, the
         * map address, not a random generator) */
        object_init();
        (void)level_load(0);
        object_setCurrent(0);
        r = object_slot(0);
        r[0] = 0x20;
        r[1] = 0x00; /* x = 0x20          */
        r[2] = 0x10;
        r[3] = 0x00; /* y = 0x10          */
        r[8] = 0x02; /* type 2            */
        {
            /* cell = 32*((0x10+0xC)/16) + (0x20+8)/16 = 32*1 + 2 = 34 */
            uint32_t cell = level_map()[34];
            check(object_mapCellEquals(cell) == -1,
                  "the map cell under the record matches");
            check(object_mapCellEquals(cell + 1u) == 0,
                  "a different cell does not");
        }
        r[8] = 0x03;
        check(object_mapCellEquals(level_map()[34]) == 0,
              "a type > 2 record is not tested against the map");
        r[8] = 0x02;
        check(object_testDispatch(0x0005u | (level_map()[34] << 8)) == 1,
              "mode 5 dispatches to the map cell");
        object_init();
    }

    /* 6c. load_terrain_b's object walk (1000:5168): level 1's four 0xAA cells
     * become the humans in static slots 0..3 and its one 0xAB cell (28,2) the
     * spear appended at index 9 — the live [0x3E73] = 9 / [0x3E5F] = 5 state */
    if (level_load(0) == 0) {
        const uint8_t *sp = object_slot(9);
        check(object_liveCount() == 9u && object_placedHumans() == 4u,
              "the walk leaves 9 used records and counts 4 humans");
        /* 0xF761's cell mapping (the score's object bonus reads it): the first
         * human sits one row above the 0xAA marker at (3,2), so the cell under
         * it -- 32*((24+0xC)/16) + (48+8)/16 -- is that 0xAA */
        check(object_recordMapCell(0u) == 0xAAu,
              "the map cell under a record is the 0xAA marker");
        check((sp[0] | (sp[1] << 8)) == 28u * 16u &&
                  (sp[2] | (sp[3] << 8)) == 2u * 16u,
              "the 0xAB cell at (28,2) appends at (448,32)");
        check((sp[4] | (sp[5] << 8)) == 0x0060u &&
                  (sp[8] | (sp[9] << 8)) == 0xFFFFu,
              "the spear's frame 0x60 and +8 = 0xFFFF");
        check(object_slotClass(1u) == 5u,
              "its behaviour 5 lands in class slot 1");
        /* D-161: the unbounded word[0x5BA4 + 2*type] read: the real entries
         * 0..2, the 0x5BAA frame table 5/6, and type 0x0F in the pause text. */
        check(object_throwDxForProbe(0u) == 8 &&
                  object_throwDxForProbe(1u) == -8 &&
                  object_throwDxForProbe(2u) == 0 &&
                  (uint32_t)object_throwDxForProbe(5u) == 0x6000u &&
                  (uint16_t)object_throwDxForProbe(6u) == 0x969Cu &&
                  object_throwDxForProbe(0x0Fu) == 0x2045,
              "the throw reads the words past 0x5BA4 (D-161)");
        /* D-162: the fourth recipe's params are 0/0/0xFFFF/0xFFFF. */
        check(object_recipeParamForProbe(3u, 0u) == 0u &&
                  object_recipeParamForProbe(3u, 1u) == 0u &&
                  object_recipeParamForProbe(3u, 2u) == 0xFFFFu &&
                  object_recipeParamForProbe(3u, 3u) == 0xFFFFu,
              "recipe 3's two 0xFFFF words are p3/p4 (D-162)");
        /* D-103: the pending-move table has 17 real pairs; the last is (2,4)
         * before the 0xFFFF sentinel. */
        check(object_pendOffForProbe(16u) == 2u &&
                  object_pendOffForProbe(17u) == 0xFFFFu &&
                  object_pendWaitForProbe(16u) == 4u,
              "the pending-move table has 17 real pairs (D-103)");
    }

    /* 6c2. the ACTION program machine (script_op_dispatch 1000:3979): level 1's
     * win is the pickup — the collision pass finds the spear, marks its slot
     * 0x85 and writes the live ordinal as the actor's class, which the goal
     * word 0x0502 (class match, argument 5) then accepts */
    if (level_load(0) == 0) {
        uint8_t *actor, *spear;

        object_setActionEntity(0u);
        actor = object_slot(0);
        spear = object_slot(9);
        check(spear != NULL && (spear[4] | (spear[5] << 8)) == 0x0060u,
              "the walk put the spear at index 9");
        /* stand the actor 8 px left of the spear, type 2 (a human) */
        actor[0] = (uint8_t)(448u - 8u);
        actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u;
        actor[3] = 0;
        actor[4] = 0x10;
        actor[6] = 0;
        actor[8] = 2;
        actor[9] = 0;
        check(object_testDispatch(0x0502u) == 0,
              "the goal is not met before the pickup");
        object_actionRun();
        check((actor[7]) == 1u, "the pickup writes the ordinal as the class");
        check(object_slotClass(1u) == 0x85u,
              "the spear's slot is marked taken (0x85)");
        check(object_testDispatch(0x0502u) == 1,
              "level 1's goal (class match, arg 5) is met after the pickup");
        /* op 4 (0xEFA1): the record's state byte must be 1 (behaviour 5, the
         * spear, whose slot 5 maps to state 1 in 0x6EB6); the pickup left
         * [0x5B56] = 2 (0x5B8A[5]), and program 2's prog[6] is op 4, so the
         * cursor at 5 selects it */
        check(object_actionProgram() == 2u, "the pickup left program 2");
        object_actionSetCursor(5u); /* program 2: prog[6] = op 4  */
        actor[8] = 0x00;
        actor[9] = 0x00; /* type <= 2 gates the op      */
        object_actionRun();
        check((actor[4] | (actor[5] << 8)) == 0x00BBu &&
                  (actor[8] | (actor[9] << 8)) == 0x0017u,
              "op 4 sets the fixed state (frame 0xBB, type 0x17)");
        object_actionSetCursor(1u);

        /* 6c2b. D-160 (closed 2026-11-04): the 0x6EB6 read window is 21
         * entries in the image (behaviours 5/6/7 -> 1/3/2, 8 -> 0x0F, 9 ->
         * 0xFF, 10..15 -> 0xC2/0x1D/0xA0/0, 16..20 -> 1) and the original
         * reads straight on into the 0/1 tables after it (0x6ECB is
         * objTypeOverlaps[8]), so the port keeps all 0x40 bytes. */
        {
            static const uint8_t want[0x15] = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x03,
                0x02, 0x0F, 0xFF, 0xC2, 0x1D, 0xA0, 0x00,
                0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
            };
            uint32_t i, bad = 0u;

            for (i = 0; i < 0x15u; i++)
                if (object_behStateForProbe(i) != want[i]) bad++;
            check(bad == 0u, "0x6EB6: the 21 state entries are the image's");
            check(object_behStateForProbe(8u) == 0x0Fu &&
                      object_behStateForProbe(16u) == 0x01u &&
                      object_behStateForProbe(20u) == 0x01u,
                  "0x6EB6: behaviours 8 and 16..20 are in the window");
            check(object_behStateForProbe(0x15u) == 0x00u &&
                      object_behStateForProbe(0x16u) == 0x01u,
                  "0x6EB6: the window continues into objTypeOverlaps[8..]");
        }
        /* the same table through op 4 (1000:EFA1's `objStateIs(actor, 1)`):
         * behaviour 16's state is 1, so the pose is applied; behaviour 13's
         * is 0, so it is not.  (Before D-160's fix both read past the 8-byte
         * array into whatever followed.) */
        object_setSlotClass(1u, 16u);
        actor[4] = 0x10;
        actor[5] = 0;
        actor[8] = 0;
        actor[9] = 0;
        object_actionSetCursor(5u);
        object_actionRun();
        check((actor[4] | (actor[5] << 8)) == 0x00BBu &&
                  (actor[8] | (actor[9] << 8)) == 0x0017u,
              "op 4 applies for behaviour 16 (state 1)");
        object_setSlotClass(1u, 13u);
        actor[4] = 0x10;
        actor[5] = 0;
        actor[8] = 0;
        actor[9] = 0;
        object_actionSetCursor(5u);
        object_actionRun();
        check((actor[4] | (actor[5] << 8)) == 0x0010u &&
                  (actor[8] | (actor[9] << 8)) == 0x0000u,
              "op 4 does not apply for behaviour 13 (state 0)");
        object_actionSetCursor(1u);
        object_setSlotClass(1u, 0x85u); /* the spear's taken mark back   */

        /* op 7 (0x39CF) is dispatched when a cursor > 1 selects it (program 3,
         * prog[2+1] = 0x07): it records the fault and touches nothing.  The
         * osys is detached for this block because the game attaches the arch's
         * fatal route (which halts) — the headless run must survive to check
         * the flag and the untouched record. */
        {
            uint8_t before[0x12];
            memcpy(before, actor, sizeof before);
            object_attachOsys(NULL);
            object_actionSetProgram(3u);
            object_actionSetCursor(2u); /* prog[3] = 0x07 */
            object_actionRun();
            check(object_actionFault() == 7u,
                  "op 7 is recorded when dispatched");
            check(memcmp(before, actor, sizeof before) == 0,
                  "op 7 touches no state yet (the ride calls are D-92)");
            object_actionSetProgram(1u);
            object_actionSetCursor(1u);
            object_attachOsys(&osys_null); /* the game's halt route back   */
        }


        /* and the level's own flow ends it as won */
        object_pendingRun();
        level_begin();
        for (uint32_t t = 0; t < 4000u && level_starting(); t++) {
            level_tick();
            palette_tick();
            if (level_loadingScreen()) level_loadingTick();
        }
        level_tick();
        /* D-48: 1000:34E6 freezes the world for 15 frames before the caption */
        for (uint32_t h = 0; h < 15u && level_endHolding(); h++) level_tick();
        check(level_ended() && level_outcome(),
              "the pickup ends level 1 as won");

        /* op 9 (0x39E4 -> 0xEF0C) + object_find_overlap (0xF2E0): a behaviour-6
         * actor walks the live record with slot 3 in its upper-left window,
         * marks it type 1, then converts the static record overlapping it */
        if (level_load(0) == 0) {
            uint8_t *mv, *live, *victim;
            object_actionSetProgram(5u); /* program 5: prog[2] = op 9 */
            object_actionSetCursor(1u);
            object_setSlotClass(2u, 6u); /* class 2 -> behaviour 6    */
            object_setSlotClass(1u, 3u); /* live ordinal 1 -> behav 3 */
            mv = object_slot(0);
            mv[0] = (uint8_t)440u;
            mv[1] = (uint8_t)(440u >> 8);
            mv[2] = 32u;
            mv[3] = 0;
            mv[4] = 0x10;
            mv[6] = 0;
            mv[7] = 2u;
            mv[8] = 0;
            mv[9] = 0;
            victim = object_slot(1); /* a neighbour at the same x */
            victim[0] = (uint8_t)440u;
            victim[1] = (uint8_t)(440u >> 8);
            victim[2] = 32u;
            victim[3] = 0;
            victim[4] = 0x10;
            victim[6] = 0;
            victim[7] = 0;
            victim[8] = 2u;
            victim[9] = 0;
            live = object_slot(9); /* the spear, +8 px right  */
            check((live[4] | (live[5] << 8)) == 0x0060u,
                  "the live record is the spear");
            object_actionRun();
            check((live[8] | (live[9] << 8)) == 0x0001u &&
                      (live[10] | (live[11] << 8)) == 0x0003u,
                  "op 9 marks the walked live record as state 1");
            check((victim[4] | (victim[5] << 8)) == 0x0103u &&
                      (victim[8] | (victim[9] << 8)) == 0x001Eu &&
                      (victim[10] | (victim[11] << 8)) == 0x0006u &&
                      (victim[14] | (victim[15] << 8)) == 0x000Au,
                  "the overlap fixup converts the overlapping record");
        }

        /* op 3 (0xECFE) with its page gate: level 1's cells (22,1) and (23,1)
         * hold ids 0x1E/0x1F (free) while (18,3) is free and its right
         * neighbour (19,3) is a wall (0x07), so a carrier on the first two
         * enters the open state and one before the wall does not.  "free" is
         * the 0x92A0 row's 16 negative bytes (`1000:F123`), built from the
         * atlas (1000:505C) - not the old field-P table. */
        check(level_pageFree(0x1Eu) == 1 && level_pageFree(0x1Fu) == 1 &&
                  level_pageFree(0x07u) == 0 && level_pageFree(0xB8u) == 0,
              "the page gate: free tiles vs walls (and 0xB8, an item tile)");
        {
            uint8_t *mv;
            object_init();
            object_actionSetProgram(1u);
            object_actionSetCursor(5u); /* prog[6] = 0x03 (op 3)     */
            mv = object_slot(0);
            mv[0] = (uint8_t)(22u * 16u);
            mv[1] = (uint8_t)((22u * 16u) >> 8);
            mv[2] = (uint8_t)(1u * 16u);
            mv[3] = (uint8_t)((1u * 16u) >> 8);
            mv[4] = 0x10;
            mv[6] = 0;
            mv[7] = 1u; /* a class -> the op runs */
            mv[8] = 0;
            mv[9] = 0; /* type 0 -> step +1     */
            object_actionRun();
            check((mv[4] | (mv[5] << 8)) == 0x008Cu &&
                      (mv[8] | (mv[9] << 8)) == 0x0012u,
                  "op 3 moves a carrier on open ground (0x8C/0x12)");
            /* (18,3) is free but its right neighbour (19,3) is a wall */
            object_init();
            object_actionSetProgram(1u);
            object_actionSetCursor(5u);
            mv = object_slot(0);
            mv[0] = (uint8_t)(18u * 16u);
            mv[1] = (uint8_t)((18u * 16u) >> 8);
            mv[2] = (uint8_t)(3u * 16u);
            mv[3] = (uint8_t)((3u * 16u) >> 8);
            mv[4] = 0x10;
            mv[6] = 0;
            mv[7] = 1u;
            mv[8] = 0;
            mv[9] = 0;
            object_actionRun();
            check((mv[4] | (mv[5] << 8)) == 0x0010u &&
                      (mv[8] | (mv[9] << 8)) == 0x0000u,
                  "op 3 refuses when the neighbour is a wall");
            object_init();
        }
    }

    /* 6d. the object pass (render_objects 1000:5D56 -> 0x653C): the four
     * placed records' base sprites, drawn with the measured storage law */
    if (level_load(0) == 0) {
        static uint8_t page[320u * 200u];
        uint8_t *bank = NULL;
        uint32_t bankSize = 0;
        uint8_t *rec;

        object_init();
        /* the level's own four placed humans, as the live game has them */
        for (uint32_t i = 0; i < 4u; i++) {
            static const uint16_t pos[4][2] = {
                { 48, 25 }, { 208, 25 }, { 256, 137 }, { 464, 265 }
            };
            rec = object_slot(i);
            rec[0] = (uint8_t)pos[i][0];
            rec[1] = (uint8_t)(pos[i][0] >> 8);
            rec[2] = (uint8_t)pos[i][1];
            rec[3] = (uint8_t)(pos[i][1] >> 8);
            rec[4] = 0x10;
            rec[6] = 0x00; /* active */
            rec[8] = 0x02;
        }
        bank = res_loadChunk("APL\\SPRITES.DAT", &bankSize, NULL);
        if (!bank || bankSize < 0x4800u + 0x180u) {
            fprintf(stderr, "FAIL SPRITES.DAT unreadable\n");
            bad = 1;
        } else {
            memset(page, 0, sizeof page);
            object_publishBank(bank, bankSize);
            object_render(page, 0, 0);
            /* the first human's visible pixels must equal the measured law */
            {
                uint32_t ink = 0, wrong = 0;
                for (uint32_t row = 0; row < 24u; row++)
                    for (uint32_t col = 0; col < 16u; col++) {
                        uint8_t v = bank[0x4800u + (col & 3u) * 0x60u +
                                         row * 4u + (col >> 2)];
                        uint8_t got = page[(25u + row) * 320u + 48u + col];
                        if (v) {
                            ink++;
                            if (got != v) wrong++;
                        } else if (got) wrong++;
                    }
                check(
                    ink > 200u && wrong == 0u,
                    "the object pass draws the caveman with the measured law");
            }
            /* the non-24-row classes: the spear (frame 0x60, h 16) uses
             * block = h*4 = 0x40, at SPRITES.DAT + 0x10A0 */
            {
                uint32_t wrong = 0;
                memset(page, 0, sizeof page);
                object_init();
                rec = object_slot(0);
                rec[0] = 10;
                rec[1] = 0;
                rec[2] = 10;
                rec[3] = 0;
                rec[4] = 0x60;
                rec[6] = 0;
                rec[8] = 0x02;
                object_render(page, 0, 0);
                for (uint32_t row = 0; row < 16u; row++)
                    for (uint32_t col = 0; col < 16u; col++) {
                        uint8_t v = bank[0x10A0u + (col & 3u) * 0x40u +
                                         row * 4u + (col >> 2)];
                        if (page[(10u + row) * 320u + 10u + col] != v) wrong++;
                    }
                check(wrong == 0u,
                      "the spear frame (h=16, block h*4) is drawn");
                /* the generated frame table now carries every slot, so the
                 * carried frames draw too -- and the descriptor's ybias is added
                 * to y (select_object_tile 1000:6642: ADD DX,AX) */
                {
                    uint32_t bad2 = 0;
                    memset(page, 0, sizeof page);
                    object_init();
                    rec = object_slot(0);
                    rec[0] = 20;
                    rec[1] = 0;
                    rec[2] = 20;
                    rec[3] = 0;
                    rec[4] = 0x5D;
                    rec[6] = 0;
                    rec[8] = 0x02;
                    object_render(page, 0, 0);
                    for (uint32_t row = 0; row < 11u; row++)
                        for (uint32_t col = 0; col < 16u; col++) {
                            uint8_t v = bank[0x0DF0u + (col & 3u) * 44u +
                                             row * 4u + (col >> 2)];
                            if (page[(22u + row) * 320u + 20u + col] != v)
                                bad2++;
                        }
                    check(bad2 == 0u && page[21u * 320u + 20u] == 0u,
                          "frame 0x5D (carried) draws at y + ybias");
                }
                /* mode 2 (1000:6C74): 32 px wide, block rows*8, row stride 8 --
                 * frame 0x2B is rows 20 at SPRITES.DAT + 0xD930 */
                {
                    uint32_t bad3 = 0;
                    memset(page, 0, sizeof page);
                    object_init();
                    rec = object_slot(0);
                    rec[0] = 40;
                    rec[1] = 0;
                    rec[2] = 40;
                    rec[3] = 0;
                    rec[4] = 0x2B;
                    rec[6] = 0;
                    rec[8] = 0x02;
                    object_render(page, 0, 0);
                    for (uint32_t row = 0; row < 20u; row++)
                        for (uint32_t col = 0; col < 32u; col++) {
                            uint8_t v = bank[0xD930u + (col & 3u) * 160u +
                                             row * 8u + (col >> 2)];
                            /* y + ybias: frame 0x2B carries ybias 4 */
                            if (page[(44u + row) * 320u + 40u + col] != v)
                                bad3++;
                        }
                    check(bad3 == 0u, "frame 0x2B (mode 2) draws 32 px wide");
                }
                /* 1000:6421 -> 1000:C23C: the placed records render in the
                 * partitioned order.  The acting record is last in its class
                 * (the climber over the member it climbs), and the type 6..8
                 * (stack) class is drawn after the live records. */
                {
                    static uint8_t soloA[320u * 200u], soloB[320u * 200u];
                    uint32_t contested = 0, won = 0;

                    object_init();
                    object_publishBank(bank, bankSize);
                    rec = object_slot(0);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x10;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    rec[8] = 7;
                    rec[9] = 0;
                    memset(soloA, 0, sizeof soloA);
                    object_render(soloA, 0, 0);

                    object_init();
                    object_publishBank(bank, bankSize);
                    rec = object_slot(1);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x2B;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    rec[8] = 7;
                    rec[9] = 0;
                    memset(soloB, 0, sizeof soloB);
                    object_render(soloB, 0, 0);

                    object_init();
                    object_publishBank(bank, bankSize);
                    rec = object_slot(0);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x10;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    rec[8] = 7;
                    rec[9] = 0;
                    rec = object_slot(1);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x2B;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    rec[8] = 7;
                    rec[9] = 0;
                    object_setActionEntity(0u); /* the climber */
                    memset(page, 0, sizeof page);
                    object_render(page, 0, 0);
                    for (uint32_t k = 0; k < 320u * 200u; k++) {
                        if (!soloA[k] || !soloB[k] || soloA[k] == soloB[k])
                            continue;
                        contested++;
                        if (page[k] == soloA[k]) won++;
                    }
                    check(contested > 20u && won == contested,
                          "the acting record draws last in its class");
                }
                {
                    static uint8_t soloA[320u * 200u], soloB[320u * 200u];
                    uint32_t contaminated = 0, bad2 = 0;

                    /* a type-7 member (front list) over a live record 8 */
                    object_init();
                    object_publishBank(bank, bankSize);
                    rec = object_slot(1);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x10;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    rec[8] = 7;
                    rec[9] = 0;
                    memset(soloA, 0, sizeof soloA);
                    object_render(soloA, 0, 0);

                    object_init();
                    object_publishBank(bank, bankSize);
                    rec = object_slot(8);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x2B;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    memset(soloB, 0, sizeof soloB);
                    object_render(soloB, 0, 0);

                    object_init();
                    object_publishBank(bank, bankSize);
                    rec = object_slot(1);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x10;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    rec[8] = 7;
                    rec[9] = 0;
                    rec = object_slot(8);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x2B;
                    rec[5] = 0;
                    rec[6] = 0;
                    rec[7] = 0;
                    memset(page, 0, sizeof page);
                    object_render(page, 0, 0);
                    for (uint32_t k = 0; k < 320u * 200u; k++)
                        if (soloA[k] && soloB[k] && soloA[k] != soloB[k]) {
                            contaminated++;
                            if (page[k] != soloA[k]) bad2++;
                        }
                    check(contaminated > 20u && bad2 == 0u,
                          "the stack class draws after the live records");
                }
                /* D-58: a climb frame (0x23..0x25) has 0x3570 bit 7 set;
                 * 1000:653C moves the base draw to 1000:6610/661C, so the
                 * placed record still draws (the climbing human is visible). */
                {
                    uint32_t ink = 0;

                    memset(page, 0, sizeof page);
                    object_init();
                    rec = object_slot(0);
                    rec[0] = 100;
                    rec[1] = 0;
                    rec[2] = 100;
                    rec[3] = 0;
                    rec[4] = 0x23;
                    rec[6] = 0;
                    rec[7] = 0;
                    rec[8] = 0x02;
                    object_render(page, 0, 0);
                    for (uint32_t k = 0; k < 320u * 200u; k++)
                        if (page[k]) ink++;
                    check(ink > 50u, "a climb frame (0x23) still draws ink");
                }
                /* 1000:E3F6, the command dispatch: the type row (DS:0x698A)
                 * picks the handler index, the table at DS:0x6918 the address.
                 * Type 2 is a human, type 8 one of the walkers, type 15 has its
                 * own tail handler instead of the common 0xC7D9. */
                {
                    ObjCommand c;

                    object_init();
                    rec = object_slot(0);
                    rec[8] = 0x02;
                    rec[9] = 0x00;
                    c = object_commandSelect(rec,
                                             0x02); /* bit 1 -> command 4 */
                    check(c.handler == 0xE609u && c.command == 4u &&
                              c.tail == 0xD9C7u,
                          "type 2 + bit 1 -> 1000:E609, the push's walk-up");
                    rec[8] = 0x08;
                    c = object_commandSelect(rec,
                                             0x01); /* bit 0 -> command 0 */
                    check(c.handler == 0xE963u && c.command == 0u,
                          "type 8 + bit 0 -> 1000:E963 (the snap)");
                    rec[8] = 0x0F;
                    c = object_commandSelect(rec, 0x02);
                    check(
                        c.handler == 0u && c.tail == 0xDC1Fu,
                        "type 15 + command 4 -> no handler, the row's own tail");
                    /* the one-shot command (key 0x48 = bit 6 + command 8) counts
                     * down from 0x4B0 and turns the record into a human (type 2) */
                    object_init();
                    rec = object_slot(0);
                    rec[8] = 0x00;
                    rec[9] = 0x00;
                    object_setCommandTimer(0x3E8u); /* 1000:41A7's arm */
                    for (uint32_t i = 0; i < 1000u; i++)
                        (void)object_commandSelect(rec, 0x00);
                    check(rec[8] == 0x02u && rec[9] == 0x00u,
                          "the 0x6DEA one-shot expires to type 2");
                    /* D-75: with the timer at 0 the type-2 idle stream runs
                     * (0xE7C0); its first hold is 200 ticks, then frame 0x12 */
                    rec[4] = 0x10;
                    rec[5] = 0;
                    for (uint32_t i = 0; i < 2u; i++)
                        (void)object_commandRun(rec, 0x00u);
                    check(object_actionPending() == 0u,
                          "the type-2 stream is reached with [0x6DEA] = 0");
                    {
                        uint32_t seen = 0, i;

                        for (i = 0; i < 700u; i++) {
                            uint32_t f;

                            (void)object_commandRun(rec, 0x00u);
                            f = rec[4] | (rec[5] << 8);
                            if (f >= 0x12u && f <= 0x16u) seen |= 1u << f;
                        }
                        check(
                            seen != 0u,
                            "the idle stream reaches the 0x12..0x16 yawn frames");
                    }
                }
                /* The human stack, end to end (re_map.md: 0x6F22).  Op 1
                 * registers the base (frame 0x20, type 8), 1000:E697 pushes a
                 * human standing just above it (frame 0x25, type 7), 1000:FDED
                 * pops the top member, and 1000:F453 + 1000:FA13 dissolve the
                 * group - which is what exercises the n >= 1 handlers. */
                {
                    uint8_t *base, *top, *below;

                    object_init();
                    base = object_slot(0);
                    base[0] = 100;
                    base[1] = 0;
                    base[2] = 100;
                    base[3] = 0;
                    base[4] = 0x10;
                    base[6] = 0;
                    base[8] = 2;
                    base[9] = 0;
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u); /* prog[3] = op 1 */
                    object_actionSetCursor(2u);
                    object_actionRun();
                    check((base[4] | (base[5] << 8)) == 0x0020u &&
                              (base[8] | (base[9] << 8)) == 0x0008u,
                          "op 1 registers the base (frame 0x20, type 8)");
                    top = object_slot(1);
                    top[0] = 100;
                    top[1] = 0;
                    top[2] = 70;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_setActionEntity(1u);
                    check(object_stackPush(top) == 1,
                          "the push takes the human standing above the base");
                    check(
                        (top[4] | (top[5] << 8)) == 0x0025u &&
                            (top[8] | (top[9] << 8)) == 0x0007u &&
                            (top[0xE] | (top[0xF] << 8)) != 0u,
                        "the pushed human is frame 0x25 / type 7 with its slot");
                    /* the pop: a caller x far from the owner truncates the list
                     * at the top member and returns the record below it */
                    check(object_stackPop(200u, &below, NULL) == 1 &&
                              below == base,
                          "the pop removes the top member and names the base");
                    check(object_stackScanDissolve(1u) == 0,
                          "the popped record is no longer in any group");
                    /* push again and dissolve: n = 1 restores the member */
                    check(object_stackPush(top) == 1, "the push works again");
                    check(
                        object_stackScanDissolve(1u) == 1,
                        "the scan finds and dissolves the group holding rec 1");
                    check((top[4] | (top[5] << 8)) == 0x0010u &&
                              (top[8] | (top[9] << 8)) == 0x0003u,
                          "n = 1 dissolves the member to frame 0x10 / type 3");
                    check((base[4] | (base[5] << 8)) == 0x0020u,
                          "the base is untouched by an n = 1 dissolve");
                }
                /* 1000:D76F with three humans - the animator's first active
                 * case (m = 2).  Only records whose frame is 0x20..0x22 are
                 * moved (1000:D838), and the offsets belong to the *backward*
                 * walk order: the base (list byte 0) takes run[len] = 0, so a
                 * plain 3-stack (base 0x20, members 0x25) must not shear. */
                {
                    uint8_t *b3, *p1, *p2, *g;
                    uint8_t pal3[768];
                    int32_t bMin = 999, bMax = -999, mMin = 999, mMax = -999;
                    int32_t tMin = 999, tMax = -999;

                    memset(pal3, 0, sizeof pal3);
                    object_init();
                    b3 = object_slot(0);
                    memset(b3, 0, 0x12u);
                    tf_wr16(b3, 0u, 100);
                    tf_wr16(b3, 2u, 100);
                    tf_wr16(b3, 4u, 0x10u);
                    tf_wr16(b3, 8u, 2u);
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u);
                    object_actionSetCursor(2u);
                    object_actionRun(); /* op 1 */
                    p1 = object_slot(1);
                    memset(p1, 0, 0x12u);
                    tf_wr16(p1, 0u, 100);
                    tf_wr16(p1, 2u, 70);
                    tf_wr16(p1, 4u, 0x10u);
                    tf_wr16(p1, 8u, 2u);
                    object_setActionEntity(1u);
                    check(object_stackPush(p1) == 1, "the 3-stack: member 1");
                    p2 = object_slot(2);
                    memset(p2, 0, 0x12u);
                    tf_wr16(p2, 0u, 100);
                    tf_wr16(p2, 2u, 52);
                    tf_wr16(p2, 4u, 0x10u);
                    tf_wr16(p2, 8u, 2u);
                    object_setActionEntity(2u);
                    check(object_stackPush(p2) == 1, "the 3-stack: member 2");
                    g = object_groupAt(0u);
                    check(g != NULL && (g[4] & 0x80u) != 0u,
                          "the group entry starts from the 0xFF clear");
                    object_animatorReset();
                    for (uint32_t k = 0; k < 60u; k++) {
                        int32_t x;

                        object_tick(pal3);
                        x = (int32_t)(int16_t)tf_rd16(b3, 0u);
                        if (x < bMin) bMin = x;
                        if (x > bMax) bMax = x;
                        x = (int32_t)(int16_t)tf_rd16(p1, 0u);
                        if (x < mMin) mMin = x;
                        if (x > mMax) mMax = x;
                        x = (int32_t)(int16_t)tf_rd16(p2, 0u);
                        if (x < tMin) tMin = x;
                        if (x > tMax) tMax = x;
                    }
                    check(bMin == 100 && bMax == 100,
                          "the stack's base stays at the entry x");
                    check(mMin == 100 && mMax == 100 && tMin == 100 &&
                              tMax == 100,
                          "the 3-stack does not shear while it animates");
                    check((g[4] & 0x80u) != 0u,
                          "the state byte keeps the 0xF0 mask");
                    check((g[4] & 0x0Fu) != 0u,
                          "the animator advanced the entry's frame index");
                }

                /* 1000:E8DD's y mismatch descends the member: 1000:E934 jumps
                 * to the *down* stepper (y += 1, frames 0x23..0x25) - the
                 * step-off's 1000:E8D4 is the one that uses the up stepper.  A
                 * held key walks the member down the column until it reaches
                 * the entry's y, where it steps off. */
                {
                    uint8_t *b4, *m1, *top;
                    uint32_t seen = 0;
                    int32_t outside = 0;

                    object_init();
                    b4 = object_slot(0);
                    memset(b4, 0, 0x12u);
                    tf_wr16(b4, 0u, 100);
                    tf_wr16(b4, 2u, 100);
                    tf_wr16(b4, 4u, 0x10u);
                    tf_wr16(b4, 8u, 2u);
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u);
                    object_actionSetCursor(2u);
                    object_actionRun();
                    m1 = object_slot(1);
                    memset(m1, 0, 0x12u);
                    tf_wr16(m1, 0u, 100);
                    tf_wr16(m1, 2u, 70);
                    tf_wr16(m1, 4u, 0x10u);
                    tf_wr16(m1, 8u, 2u);
                    object_setActionEntity(1u);
                    check(object_stackPush(m1) == 1,
                          "the column for the leave");
                    top = object_slot(2);
                    memset(top, 0, 0x12u);
                    tf_wr16(top, 0u, 100);
                    tf_wr16(top, 2u, 52);
                    tf_wr16(top, 4u, 0x10u);
                    tf_wr16(top, 8u, 2u);
                    object_setActionEntity(2u);
                    check(object_stackPush(top) == 1,
                          "the third joins the column");
                    for (uint32_t k = 0; k < 20u; k++) {
                        uint32_t f;

                        (void)object_commandRun(top, 0x01u);
                        f = (uint32_t)(top[4] | (top[5] << 8));
                        if (f < 0x23u || f > 0x25u) outside = 1;
                        seen |= 1u << (f & 0x1Fu);
                    }
                    check(!outside && (seen & (1u << 3)) != 0u &&
                              (seen & (1u << 4)) != 0u &&
                              (seen & (1u << 5)) != 0u,
                          "1000:E934: a member climbs down 0x23..0x25");
                    /* the push lifted it from 52 to 53 (1000:E6DA), then one
                     * pixel per press */
                    check(tf_rd16(top, 2u) == 52u + 1u + 20u,
                          "1000:E934: the down stepper moves y +1 a press");
                    tf_wr16(top, 2u, 100u);
                    check(object_stackLeave(top) == 1 &&
                              tf_rd16(top, 8u) == 2u &&
                              (top[4] | (top[5] << 8)) == 0x10u,
                          "the leave at the entry's y restores the human");
                }

                /* The other four verbs: grab (1000:E58B, with its free-slot
                 * registration), step off (1000:E860), leave (1000:E8DD),
                 * snap (1000:E963) and the selector (1000:F17C).  The 0x64AD /
                 * 0x64C1 masks are written by the pushes and read by the
                 * selector's 0x6F22 check. */
                {
                    uint8_t *base, *top;

                    object_init();
                    base = object_slot(0);
                    base[0] = 100;
                    base[1] = 0;
                    base[2] = 100;
                    base[3] = 0;
                    base[4] = 0x10;
                    base[6] = 0;
                    base[8] = 2;
                    base[9] = 0;
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u);
                    object_actionSetCursor(2u);
                    object_actionRun(); /* op 1: the base */
                    top = object_slot(1);
                    top[0] = 102;
                    top[1] = 0;
                    top[2] = 101;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_setActionEntity(1u);
                    check(object_stackGrab(top) == 1,
                          "the grab takes a human within 8 x / 4 y");
                    check(
                        (top[4] | (top[5] << 8)) == 0x0023u &&
                            (top[8] | (top[9] << 8)) == 0x0006u &&
                            (top[2] | (top[3] << 8)) == 100u,
                        "the grabbed human is frame 0x23 / type 6 at the base x");
                    /* snap: the grabbed record is the top member */
                    object_init();
                    base = object_slot(0);
                    base[0] = 100;
                    base[1] = 0;
                    base[2] = 100;
                    base[3] = 0;
                    base[4] = 0x10;
                    base[6] = 0;
                    base[8] = 2;
                    base[9] = 0;
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u);
                    object_actionSetCursor(2u);
                    object_actionRun();
                    top = object_slot(1);
                    top[0] = 100;
                    top[1] = 0;
                    top[2] = 70;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_setActionEntity(1u);
                    check(object_stackPush(top) == 1, "the push for the verbs");
                    top[0] = 120; /* off the base's x */
                    check(object_stackSnap(top) == 1 &&
                              (top[0] | (top[1] << 8)) == 100u &&
                              (top[4] | (top[5] << 8)) == 0x0020u,
                          "the snap puts the top member on the base's x");
                    /* step off: on `entry.y - 0x18 == actor.y` the actor and the
                     * list byte below it both move down 8 */
                    top[2] = 76;
                    top[3] = 0;
                    check(object_stackStepOff(top) == 1 &&
                              (top[2] | (top[3] << 8)) == 84u &&
                              (top[4] | (top[5] << 8)) == 0x0020u &&
                              (top[8] | (top[9] << 8)) == 8u,
                          "the step off moves the actor down 8 (frame 0x20)");
                    check((base[2] | (base[3] << 8)) == 108u &&
                              (base[4] | (base[5] << 8)) == 0x0021u,
                          "the step off moves the record below it too");
                    /* leave: on `entry.y == actor.y` the actor is a standing
                     * human again and the list is cut at its stored slot */
                    top[2] = 100;
                    top[3] = 0;
                    check(object_stackLeave(top) == 1 &&
                              (top[4] | (top[5] << 8)) == 0x0010u &&
                              (top[8] | (top[9] << 8)) == 2u,
                          "the leave restores the actor (frame 0x10 / type 2)");
                    check(object_stackScanDissolve(1u) == 0,
                          "the leave cut the list at the actor's slot");
                    /* 1000:E99D (index 15): stand back on the group - the
                     * topmost member takes the entry's x, y-7, frame 0x25,
                     * and the record below moves up 8 with frame 0x20 */
                    object_init();
                    base = object_slot(0);
                    base[0] = 100;
                    base[1] = 0;
                    base[2] = 100;
                    base[3] = 0;
                    base[4] = 0x10;
                    base[6] = 0;
                    base[8] = 2;
                    base[9] = 0;
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u);
                    object_actionSetCursor(2u);
                    object_actionRun();
                    top = object_slot(1);
                    top[0] = 100;
                    top[1] = 0;
                    top[2] = 70;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_setActionEntity(1u);
                    check(object_stackPush(top) == 1,
                          "the push before the stand");
                    check(object_stackStand(top) == 1 &&
                              (top[0] | (top[1] << 8)) == 100u &&
                              (top[2] | (top[3] << 8)) ==
                                  64u && /* 71 after the push */
                              (top[4] | (top[5] << 8)) == 0x0025u &&
                              (top[8] | (top[9] << 8)) == 7u &&
                              (top[0xE] | (top[0xF] << 8)) != 0u,
                          "1000:E99D stands the member (x, y-7, frame 0x25)");
                    check((base[2] | (base[3] << 8)) == 92u &&
                              (base[4] | (base[5] << 8)) == 0x0020u,
                          "1000:E99D moves the record below up 8");
                }
                /* 1000:E896/E89B: the step off's walk starts at DX = entry.y and
                 * subtracts 0x10 for every member it passes before the match,
                 * then 0x18 at the match - so the second member's target is
                 * entry.y - 0x28, one level above the owner's 0x18. */
                {
                    uint8_t *base, *m1, *m2;

                    object_init();
                    base = object_slot(0);
                    base[0] = 100;
                    base[1] = 0;
                    base[2] = 100;
                    base[3] = 0;
                    base[4] = 0x10;
                    base[6] = 0;
                    base[8] = 2;
                    base[9] = 0;
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u);
                    object_actionSetCursor(2u);
                    object_actionRun();
                    m1 = object_slot(1);
                    m1[0] = 100;
                    m1[1] = 0;
                    m1[2] = 70;
                    m1[3] = 0;
                    m1[4] = 0x10;
                    m1[6] = 0;
                    m1[8] = 2;
                    m1[9] = 0;
                    object_setActionEntity(1u);
                    check(object_stackPush(m1) == 1,
                          "the first member for the slot walk");
                    m2 = object_slot(2);
                    m2[0] = 100;
                    m2[1] = 0;
                    m2[2] = 52;
                    m2[3] = 0;
                    m2[4] = 0x10;
                    m2[6] = 0;
                    m2[8] = 2;
                    m2[9] = 0;
                    object_setActionEntity(2u);
                    check(object_stackPush(m2) == 1,
                          "the second member for the slot walk");
                    /* negative control: the owner's y is not this slot's */
                    m2[2] = 76;
                    m2[3] = 0;
                    check(object_stackStepOff(m2) == -1 &&
                              (m1[4] | (m1[5] << 8)) == 0x0025u,
                          "1000:E896: the second member refuses the owner's y");
                    m2[2] = 60;
                    m2[3] = 0;
                    check(
                        object_stackStepOff(m2) == 1 &&
                            (m2[2] | (m2[3] << 8)) == 68u &&
                            (m2[4] | (m2[5] << 8)) == 0x0020u &&
                            (m2[8] | (m2[9] << 8)) == 8u,
                        "1000:E896: the second member steps off at entry.y - 0x28");
                    check(
                        (m1[2] | (m1[3] << 8)) == 79u &&
                            (m1[4] | (m1[5] << 8)) == 0x0021u,
                        "1000:E896: the record below the second member crouches");
                }
                /* the selector (1000:F17C): its stack path removes the member,
                 * and its two terrain-dependent branches report -2 */
                {
                    uint8_t *base, *top, *below;

                    object_init();
                    base = object_slot(0);
                    base[0] = 100;
                    base[1] = 0;
                    base[2] = 100;
                    base[3] = 0;
                    base[4] = 0x10;
                    base[6] = 0;
                    base[8] = 2;
                    base[9] = 0;
                    object_setActionEntity(0u);
                    object_actionSetProgram(1u);
                    object_actionSetCursor(2u);
                    object_actionRun();
                    top = object_slot(1);
                    top[0] = 100;
                    top[1] = 0;
                    top[2] = 70;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_setActionEntity(1u);
                    check(object_stackPush(top) == 1,
                          "the push for the selector");
                    /* frame 0x25 is not the stack pose, so a near target hands
                     * over to the walk step (1000:E6FB -> 1000:F25F, the level's
                     * two maps).  It either steps or refuses whole. */
                    {
                        uint16_t x0 = (uint16_t)(top[0] | (top[1] << 8));

                        check(object_selector(top, 1u, 8u, 1u) == 0 &&
                                  (top[4] | (top[5] << 8)) == 0x0008u &&
                                  (uint16_t)(top[0] | (top[1] << 8)) == x0,
                              "a far target only writes the frame (1000:F20C)");
                    }
                    /* in the stack pose the selector is a plain frame write */
                    check(object_stackSnap(top) == 1,
                          "the snap before the selector");
                    check(
                        object_selector(top, 1u, 8u, 1u) == 0 &&
                            (top[4] | (top[5] << 8)) == 8u,
                        "the selector writes the target frame in the stack pose");
                    /* a caller x far from the record below removes the member;
                     * the unmarked 0x64AD/0x64C1 bit then runs 1000:E389 and
                     * 1000:F223, which either takes the entry's x or stops on a
                     * special tile (field K's [0x6F20]/[0x6F21]) */
                    top[0] = 140;
                    top[1] = 0;
                    check(object_selector(top, 200u, 8u, 1u) == 0 &&
                              (top[8] | (top[9] << 8)) == 1u,
                          "the selector's removal sets the wrapper's type");
                    /* 1000:E361 aligns the probe row (AND BX,0xFFF0), so the
                     * distance test is 1 px and F1D7 returns before F223's x
                     * write; the old 100 was the unaligned reading. */
                    check(
                        (top[0] | (top[1] << 8)) == 140u,
                        "1000:F1D7 rejects the 1-px step before F223's x write");
                    check((base[2] | (base[3] << 8)) == 92u &&
                              (base[4] | (base[5] << 8)) == 0x0020u,
                          "the record below moves up 8 and takes frame 0x20");
                    below = NULL;
                    check(object_stackPop(100u, &below, NULL) == -1 &&
                              object_stackScanDissolve(1u) == 0,
                          "the selector removed the member from the group");
                }
                /* The walk step (1000:E6FB/E764) against level 1's two maps:
                 * the selector pops first, so the actor must be a topmost
                 * stack member; the step then depends on the tile above it
                 * (0x92A0's page byte) and the item record's bit (0x1FD6).
                 * Scan the map for a cell that steps and one that refuses. */
                {
                    int32_t stepped = 0, refused = 0, right = 0;

                    for (uint32_t y = 32u; y < 152u; y += 16u) {
                        for (uint32_t x = 16u; x < 304u; x += 16u) {
                            uint8_t *base, *top2;
                            uint16_t x0, f0, x1;

                            object_init();
                            base = object_slot(0);
                            base[0] = (uint8_t)x;
                            base[1] = 0;
                            base[2] = (uint8_t)y;
                            base[3] = 0;
                            base[4] = 0x10;
                            base[6] = 0;
                            base[8] = 2;
                            base[9] = 0;
                            object_setActionEntity(0u);
                            object_actionSetProgram(1u);
                            object_actionSetCursor(2u);
                            object_actionRun();
                            top2 = object_slot(1);
                            top2[0] = (uint8_t)x;
                            top2[1] = 0;
                            top2[2] = (uint8_t)(y - 0x1Cu);
                            top2[3] = 0;
                            top2[4] = 0x10;
                            top2[6] = 0;
                            top2[8] = 2;
                            top2[9] = 0;
                            object_setActionEntity(1u);
                            if (object_stackPush(top2) != 1) continue;
                            /* the first call lands the frame in the left cycle
                             * (1000:F20C); the second one is the walk's own
                             * distance test (0 <= frame - 8 < 8) */
                            if (object_selector(top2, 1u, 8u, 1u) != 0)
                                continue;
                            x0 = (uint16_t)(top2[0] | (top2[1] << 8));
                            f0 = (uint16_t)(top2[4] | (top2[5] << 8));
                            if (object_selector(top2, 1u, 8u, 1u) != 0)
                                continue;
                            x1 = (uint16_t)(top2[0] | (top2[1] << 8));
                            if (x1 == x0) {
                                check(
                                    (top2[4] | (top2[5] << 8)) == f0,
                                    "a refused walk step leaves the frame alone");
                                refused++;
                                continue;
                            }
                            check(x1 == (uint16_t)(x0 - 1u),
                                  "the left walk step moves x by one");
                            check((uint32_t)(top2[4] | (top2[5] << 8)) >= 8u &&
                                      (uint32_t)(top2[4] | (top2[5] << 8)) <=
                                          0x000Fu,
                                  "the left walk frame lands in 8..0xF");
                            stepped++;
                            /* the right walk (1000:E764) from the same spot:
                             * [0x4238] = 0 puts the frame in the 0..7 cycle */
                            (void)object_selector(top2, 1u, 0u, 0u);
                            if (object_selector(top2, 1u, 0u, 0u) == 0 &&
                                (uint16_t)(top2[0] | (top2[1] << 8)) ==
                                    (uint16_t)(x1 + 1u) &&
                                (uint32_t)(top2[4] | (top2[5] << 8)) <= 7u)
                                right++;
                        }
                    }
                    check(stepped != 0,
                          "some cell of level 1 lets the walk step");
                    check(refused != 0,
                          "some cell of level 1 refuses the walk step");
                    check(right != 0, "the right walk step mirrors the left");
                }

                /* The walk against the live original (level 1, 2026-11-01,
                 * DS = 0x123A).  From (48,24):
                 *   - left: the original stops at x = 21 (tile 0x2A's item
                 *     word blocks it - the draw_objects masks, 1000:558B);
                 *   - right: the settled y is 27 at x = 70..89 and 24 by
                 *     x = 94 (the settle masks the row base, 1000:D8D0).
                 * Both are end-to-end, through the real map and tables. */
                {
                    uint8_t *w;
                    uint32_t i, x, y;

                    object_init();
                    w = object_slot(0);
                    tf_wr16(w, 0u, 48);
                    tf_wr16(w, 2u, 24);
                    tf_wr16(w, 4u, 0x10u);
                    tf_wr16(w, 8u, 0u);
                    tf_wr16(w, 0xAu, 0u);
                    tf_wr16(w, 0xCu, 0u);
                    object_setActionEntity(0u);
                    for (i = 0; i < 40u; i++) (void)object_commandRun(w, 0x04u);
                    x = tf_rd16(w, 0u);
                    check(x == 21u, "the wall stops the left walk at x 21");

                    object_init();
                    w = object_slot(0);
                    tf_wr16(w, 0u, 48);
                    tf_wr16(w, 2u, 24);
                    tf_wr16(w, 4u, 0x10u);
                    tf_wr16(w, 8u, 0u);
                    tf_wr16(w, 0xAu, 0u);
                    tf_wr16(w, 0xCu, 0u);
                    object_setActionEntity(0u);
                    for (i = 0; i < 41u; i++) (void)object_commandRun(w, 0x08u);
                    x = tf_rd16(w, 0u);
                    y = tf_rd16(w, 2u);
                    check(x == 89u && y == 27u,
                          "the right walk settles to y 27 at x 89");
                    for (i = 0; i < 5u; i++) (void)object_commandRun(w, 0x08u);
                    x = tf_rd16(w, 0u);
                    y = tf_rd16(w, 2u);
                    check(x == 94u && y == 24u,
                          "the right walk is back at y 24 by x 94");
                }

                /* The active-human marker: the selector arms record 8
                 * (1000:4188) and update_world's 1000:D64F follows the acting
                 * record, bobs its type word and expires. */
                {
                    uint8_t *m, *h;

                    object_init();
                    h = object_slot(1);
                    memset(h, 0, 0x12u);
                    tf_wr16(h, 0u, 100);
                    tf_wr16(h, 2u, 120);
                    tf_wr16(h, 4u, 0x10u);
                    tf_wr16(h, 8u, 2u);
                    h = object_slot(2);
                    memset(h, 0, 0x12u);
                    tf_wr16(h, 0u, 200);
                    tf_wr16(h, 2u, 80);
                    tf_wr16(h, 4u, 0x10u);
                    tf_wr16(h, 8u, 2u);
                    object_setActionEntity(1u);
                    m = object_slot(8u);
                    memset(m, 0, 0x12u);
                    tf_wr16(m, 0u, 0xFFFFu);
                    tf_wr16(m, 2u, 0xFFFFu);
                    check(object_cycleEntity(1) == 2,
                          "the selector moves to the next enabled record");
                    check(tf_rd16(m, 0u) == 200u &&
                              tf_rd16(m, 2u) == 80u - 0x1Au &&
                              tf_rd16(m, 6u) == 0u && tf_rd16(m, 8u) == 0x1Au &&
                              tf_rd16(m, 0xAu) == 0x0605u,
                          "1000:4188 arms the marker on the new acting record");
                    object_markerTick();
                    check(tf_rd16(m, 0u) == 200u &&
                              tf_rd16(m, 2u) == 80u - 0x1Au,
                          "1000:D64F tracks the acting record");
                    for (uint32_t k = 0; k < 6u; k++) object_markerTick();
                    check(tf_rd16(m, 8u) == 0x18u,
                          "the marker's type steps 0x1A -> 0x18 (the bob)");
                    /* the lifetime: type 0x12 and a zero high byte expire */
                    tf_wr16(m, 8u, 0x12u);
                    tf_wr16(m, 0xAu, 0x0001u);
                    object_markerTick(); /* low byte -> 0 */
                    object_markerTick(); /* wrap: +B 0 -> -1 */
                    check((m[6] & 0x80u) != 0u,
                          "the marker disables when its +0xA lifetime ends");
                }

                /* The key list (1000:386E / DS:0x1989) and the action row:
                 * F1..F8 select record `scancode - 0x3B`, ','/'.' the
                 * previous/next record, SPACE either home-states the acting
                 * record or steps the row's selected cell (1000:3933). */
                {
                    InputState k;
                    uint8_t *r0, *r1;
                    uint32_t cell;

                    memset(&k, 0, sizeof k);
                    object_init();
                    r0 = object_slot(0);
                    memset(r0, 0, 0x12u);
                    tf_wr16(r0, 8u, 2u);
                    r1 = object_slot(1);
                    memset(r1, 0, 0x12u);
                    tf_wr16(r1, 8u, 2u);
                    level_setUpdates(1);
                    k.keyScan = 0x3Cu; /* F2 */
                    check(gameflow_keyList(&k) == 1 &&
                              object_actionEntity() == 1u,
                          "F2 selects record 1 (1000:38C3)");
                    k.keyScan = 0x3Bu; /* F1 */
                    check(gameflow_keyList(&k) == 1 &&
                              object_actionEntity() == 0u,
                          "F1 selects record 0");
                    k.keyScan = 0x34u; /* '.' next */
                    check(gameflow_keyList(&k) == 1 &&
                              object_actionEntity() == 1u,
                          "'.' steps to the next record (1000:38E4)");
                    k.keyScan = 0x33u; /* ',' previous */
                    check(gameflow_keyList(&k) == 1 &&
                              object_actionEntity() == 0u,
                          "',' steps to the previous record");
                    r1[6] = 0x80u; /* disabled */
                    k.keyScan = 0x3Cu;
                    check(gameflow_keyList(&k) == 1 &&
                              object_actionEntity() == 0u,
                          "a disabled record is not selected (1000:38CE)");
                    r1[6] = 0u;
                    /* SPACE: a convertible pose is stood up, the cell stays */
                    object_actionSetProgram(1u);
                    cell = statusbar_actionCell(1u);
                    tf_wr16(r0, 8u, 0x12u);
                    tf_wr16(r0, 4u, 0x2Bu);
                    k.keyScan = 0x39u;
                    check(gameflow_keyList(&k) == 1 && tf_rd16(r0, 8u) == 0u &&
                              statusbar_actionCell(1u) == cell,
                          "SPACE home-states the acting record (1000:392D)");
                    /* a normal pose steps the row's selected cell instead */
                    k.keyScan = 0x39u;
                    check(gameflow_keyList(&k) == 1 &&
                              statusbar_actionCell(1u) != cell,
                          "SPACE steps the action row's cell (1000:3933)");
                    /* the [0xC] gate: F2 needs the world open, F10 (id bit 7)
                     * runs regardless */
                    level_setUpdates(0);
                    k.keyScan = 0x3Cu;
                    check(gameflow_keyList(&k) == 0,
                          "the [0xC] gate stops F2 (1000:38AD)");
                    k.keyScan = 0x44u; /* F10 */
                    check(gameflow_keyList(&k) == 1 && gameflow_keyWaitActive(),
                          "F10 runs with the world gate closed (id bit 7)");
                    /* 1000:3954: release -> press -> release, then back to
                     * the play loop */
                    memset(&k, 0, sizeof k);
                    check(gameflow_keyList(&k) == 1 && gameflow_keyWaitActive(),
                          "F10 waits for the release");
                    k.lastKey = 'A'; /* a make */
                    check(gameflow_keyList(&k) == 1 && gameflow_keyWaitActive(),
                          "F10 then waits for the press");
                    memset(&k, 0, sizeof k);
                    check(gameflow_keyList(&k) == 1 &&
                              !gameflow_keyWaitActive(),
                          "F10 releases the world after the second release");
                    level_setUpdates(1);
                }

                /* The two movement states that feed the stack: 1000:E4F8 (step
                 * down) and 1000:E609 (walk up).  Their feet cell decides a
                 * special tile (field K), a 0x6F92 span, else the grab / the
                 * push.  No 0x6F92 span exists in these tests, so the tile rule
                 * and the dispatch are what is driven. */
                {
                    const uint8_t *map = level_map();
                    uint8_t *a0, *a1;
                    uint32_t sp0 = level_specialTile(0u),
                             sp1 = level_specialTile(1u);
                    int32_t found = 0, col = 0, row = 0;
                    int32_t tileUp = 0, tileDown = 0;

                    check(map != NULL,
                          "level 1's map is loaded for the states");
                    for (uint32_t r = 0; r < 640u && !found; r++)
                        if (map[r] == sp0 || map[r] == sp1) {
                            found = 1;
                            row = (int32_t)(r / 32u);
                            col = (int32_t)(r % 32u);
                        }
                    if (found) {
                        object_init();
                        a0 = object_slot(0);
                        a0[0] = (uint8_t)(16 * col - 8);
                        a0[1] = 0;
                        a0[2] = (uint8_t)(16 * row - 0x19);
                        a0[3] = 0;
                        a0[4] = 0x10;
                        a0[6] = 0;
                        a0[8] = 2;
                        a0[9] = 0;
                        check(object_stepUp(a0) == 1 &&
                                  (a0[0] | (a0[1] << 8)) == 16 * col &&
                                  (a0[2] | (a0[3] << 8)) == 16 * row - 0x18 &&
                                  (a0[4] | (a0[5] << 8)) == 0x25u &&
                                  (a0[8] | (a0[9] << 8)) == 5u,
                              "the walk-up tile rule snaps to the grid");
                        tileUp = 1;
                        object_init();
                        a0 = object_slot(0);
                        a0[0] = (uint8_t)(16 * col - 8);
                        a0[1] = 0;
                        a0[2] = (uint8_t)(16 * row - 0x16);
                        a0[3] = 0;
                        a0[4] = 0x10;
                        a0[6] = 0;
                        a0[8] = 2;
                        a0[9] = 0;
                        check(
                            object_stepDown(a0) == 1 &&
                                (a0[4] | (a0[5] << 8)) == 0x23u &&
                                (a0[8] | (a0[9] << 8)) == 4u,
                            "the step-down tile rule sets frame 0x23 / type 4");
                        tileDown = 1;
                    }
                    check(tileUp != 0 && tileDown != 0,
                          "level 1 has a special tile for both states");
                    /* 1000:E828/E857: the climb steppers' bounds are 0x23/0x25
                     * (the 0x52/0x54 pair belongs to 1000:EAAC's carry
                     * stepper).  Drive both states' tails with the feet
                     * re-pinned on the special cell so the probe keeps
                     * matching; the frame must stay inside the climb pair. */
                    if (found) {
                        uint8_t *a;
                        uint32_t seen = 0;
                        int32_t outside = 0;

                        object_init();
                        a = object_slot(0);
                        a[0] = (uint8_t)(16 * col - 8);
                        a[1] = 0;
                        a[4] = 0x23;
                        a[6] = 0;
                        a[8] = 4;
                        a[9] = 0;
                        a[0xA] = 0;
                        a[0xB] = 0;
                        a[0xC] = 0;
                        object_setActionEntity(0u);
                        for (uint32_t t = 0; t < 30u; t++) {
                            uint32_t f;

                            a[2] = (uint8_t)(16 * row - 0xE);
                            a[3] = 0;
                            (void)object_commandRun(a, 0x01u);
                            f = (uint32_t)(a[4] | (a[5] << 8));
                            if (f < 0x23u || f > 0x25u) outside = 1;
                            seen |= 1u << (f & 0x1Fu);
                        }
                        check(!outside &&
                                  (seen & (1u << (0x23u & 0x1Fu))) != 0u &&
                                  (seen & (1u << (0x24u & 0x1Fu))) != 0u &&
                                  (seen & (1u << (0x25u & 0x1Fu))) != 0u,
                              "1000:E828: the climb cycles 0x23..0x25");
                        check(tf_rd16(a, 2u) == (uint32_t)(16 * row - 0xE - 1),
                              "1000:FD91 still moves y one px per step");
                        object_init();
                        a = object_slot(0);
                        a[0] = (uint8_t)(16 * col - 8);
                        a[1] = 0;
                        a[4] = 0x25;
                        a[6] = 0;
                        a[8] = 5;
                        a[9] = 0;
                        a[0xA] = 0;
                        a[0xB] = 0;
                        a[0xC] = 0;
                        object_setActionEntity(0u);
                        outside = 0;
                        seen = 0;
                        for (uint32_t t = 0; t < 30u; t++) {
                            uint32_t f;

                            a[2] = (uint8_t)(16 * row - 0x10);
                            a[3] = 0;
                            (void)object_commandRun(a, 0x01u);
                            f = (uint32_t)(a[4] | (a[5] << 8));
                            if (f < 0x23u || f > 0x25u) outside = 1;
                            seen |= 1u << (f & 0x1Fu);
                        }
                        check(!outside &&
                                  (seen & (1u << (0x23u & 0x1Fu))) != 0u &&
                                  (seen & (1u << (0x24u & 0x1Fu))) != 0u &&
                                  (seen & (1u << (0x25u & 0x1Fu))) != 0u,
                              "1000:E857: the descend cycles 0x23..0x25");
                    }
                    /* with no group and no span: up does nothing, down
                     * registers the base (the grab's free-slot path), and the
                     * next walk-up pushes onto it */
                    object_init();
                    a0 = object_slot(0);
                    a0[0] = 100;
                    a0[1] = 0;
                    a0[2] = 100;
                    a0[3] = 0;
                    a0[4] = 0x10;
                    a0[6] = 0;
                    a0[8] = 2;
                    a0[9] = 0;
                    object_setActionEntity(0u);
                    check(object_stepUp(a0) == 4,
                          "walk-up with no group does nothing");
                    check(object_stepDown(a0) == 4 &&
                              (a0[4] | (a0[5] << 8)) == 0x0020u &&
                              (a0[8] | (a0[9] << 8)) == 8u,
                          "step-down registers the base (frame 0x20 / type 8)");
                    {
                        int32_t pushed = 0;

                        for (uint32_t ty = 68u; ty < 76u && !pushed; ty++) {
                            object_init();
                            a0 = object_slot(0);
                            a0[0] = 100;
                            a0[1] = 0;
                            a0[2] = 100;
                            a0[3] = 0;
                            a0[4] = 0x10;
                            a0[6] = 0;
                            a0[8] = 2;
                            a0[9] = 0;
                            object_setActionEntity(0u);
                            if (object_stepDown(a0) != 4) continue;
                            a1 = object_slot(1);
                            a1[0] = 100;
                            a1[1] = 0;
                            a1[2] = (uint8_t)ty;
                            a1[3] = 0;
                            a1[4] = 0x10;
                            a1[6] = 0;
                            a1[8] = 2;
                            a1[9] = 0;
                            object_setActionEntity(1u);
                            if (object_stepUp(a1) != 3) continue;
                            check((a1[4] | (a1[5] << 8)) == 0x0025u &&
                                      (a1[8] | (a1[9] << 8)) == 7u,
                                  "walk-up pushes (frame 0x25 / type 7)");
                            pushed = 1;
                        }
                        check(pushed != 0,
                              "walk-up pushes onto the registered base");
                    }
                    /* 1000:E4B0: the dispatch runs the handler, not just selects it.
                 * The movement states change the record's type, which skips the
                 * tail (1000:E4CC); a row with no handler but a ported tail
                 * reports the tail, and anything without a port says so. */
                    {
                        uint8_t *base, *a1;

                        object_init();
                        base = object_slot(0);
                        base[0] = 100;
                        base[1] = 0;
                        base[2] = 100;
                        base[3] = 0;
                        base[4] = 0x10;
                        base[6] = 0;
                        base[8] = 2;
                        base[9] = 0;
                        object_setActionEntity(0u);
                        check(object_stepDown(base) == 4, "the base registers");
                        a1 = object_slot(1);
                        a1[0] = 100;
                        a1[1] = 0;
                        a1[2] = 70;
                        a1[3] = 0;
                        a1[4] = 0x10;
                        a1[6] = 0;
                        a1[8] = 2;
                        a1[9] = 0;
                        object_setActionEntity(1u);
                        check(object_commandRun(a1, 0x02u) == OBJ_CMD_HANDLER,
                              "type 2 + bit 1 runs the walk-up handler");
                        check((a1[8] | (a1[9] << 8)) != 2u,
                              "...whose type change skips the row's tail");
                        /* command 2 -> the type-2 row's index 6 (1000:E790, the
                     * idle handler): frame 0x10/type 2 -> 0/0 with a 4-frame
                     * countdown, a type change, so the tail is skipped */
                        a1[8] = 2;
                        a1[9] = 0;
                        a1[4] = 0x10;
                        a1[5] = 0;
                        a1[0xA] = 0;
                        a1[0xB] = 0;
                        check(
                            object_commandRun(a1, 0x08u) == OBJ_CMD_HANDLER &&
                                (a1[4] | (a1[5] << 8)) == 0u &&
                                (a1[8] | (a1[9] << 8)) == 0u &&
                                (a1[0xA] | (a1[0xB] << 8)) == 4u,
                            "command 2 runs the idle handler (frame 0 / type 0)");
                        /* command 6 -> index 4 (1000:E72A): 0x10 -> 8 / type 1 */
                        a1[8] = 2;
                        a1[9] = 0;
                        a1[4] = 0x10;
                        a1[5] = 0;
                        a1[0xA] = 0;
                        a1[0xB] = 0;
                        check(
                            object_commandRun(a1, 0x04u) == OBJ_CMD_HANDLER &&
                                (a1[4] | (a1[5] << 8)) == 8u &&
                                (a1[8] | (a1[9] << 8)) == 1u,
                            "command 6 runs the other idle handler (frame 8 / type 1)");
                        /* every row's tail is ported now: drive each type's
                     * command 0 (input 0x01) and require that none reports an
                     * unported handler or tail. */
                        {
                            uint32_t unported = 0;

                            for (uint32_t t = 0; t < 35u; t++) {
                                a1[8] = (uint8_t)t;
                                a1[9] = 0;
                                a1[0xA] = 0;
                                a1[0xB] = 0;
                                a1[0xE] = 0;
                                a1[0xF] = 0;
                                if (object_commandRun(a1, 0x01u) ==
                                    OBJ_CMD_UNPORTED)
                                    unported++;
                            }
                            check(unported == 0u,
                                  "every one of the 35 rows runs to its tail");
                        }
                        /* D-16: type 2 command 8 (1000:E7C0) walks the timed
                     * frame stream; the one-shot gate is cleared for the seam. */
                        a1[8] = 2;
                        a1[9] = 0;
                        a1[4] = 0;
                        a1[5] = 0;
                        object_setCommandTimer(0u);
                        object_streamCommand(a1);
                        check(
                            (a1[4] | (a1[5] << 8)) == 0x10u,
                            "1000:E7C0: type 2 command 8 takes the first stream frame");
                        /* type 15 command 8 (1000:EBCA) clears the +0xA hold */
                        a1[8] = 15;
                        a1[9] = 0;
                        a1[0xA] = 5;
                        a1[0xB] = 0;
                        object_clearCountdown(a1);
                        check((a1[0xA] | (a1[0xB] << 8)) == 0u,
                              "1000:EBCA: type 15 command 8 clears the hold");
                        check(object_commandRun(a1, 0x00u) != OBJ_CMD_UNPORTED,
                              "1000:EBCA is wired into the command table");
                        /* 1000:D6B2 phases 1/2: the pending-move table raises a
                     * record's +4 tag 0x21 -> 0x22 for 0x28 ticks and puts it
                     * back. */
                        {
                            uint8_t *p = object_slot(1);

                            if (p && object_placedHumans() > 1u) {
                                p[4] = 0x21;
                                p[5] = 0;
                                object_animatorReset();
                                object_animatorTick();
                                check((p[4] | (p[5] << 8)) == 0x22u,
                                      "1000:D6B2 phase 1 raises 0x21 -> 0x22");
                                for (uint32_t i = 0; i < 0x29u; i++)
                                    object_animatorTick();
                                check(
                                    (p[4] | (p[5] << 8)) == 0x21u,
                                    "1000:D6B2 phase 2 restores 0x22 -> 0x21");
                            }
                        }

                        /* 1000:D6B2 phase 3: three members on a group animate
                     * their x with the length-3 count-0 offsets.  The list is
                     * walked backwards from the terminator while the run is
                     * read forwards (1000:D81D/D820), so the top member (27)
                     * takes run[1] = -2, 26 takes run[2] = -1 and the owner
                     * (25) takes run[3] = 0. */
                        {
                            uint8_t *g = object_groupAt(7);
                            uint8_t *m0 = object_slot(25);
                            uint8_t *m1 = object_slot(26);
                            uint8_t *m2 = object_slot(27);

                            if (g && m0 && m1 && m2) {
                                memset(g, 0, 14u);
                                g[0] = 100;
                                g[1] = 0; /* group base x */
                                g[4] = 0x80; /* the 0xFF clear's
                                                          * sentinel, dir 0 */
                                g[5] = 25;
                                g[6] = 26;
                                g[7] = 27;
                                g[8] = 0x80;
                                m0[4] = m1[4] = m2[4] = 0x20;
                                m0[5] = m1[5] = m2[5] = 0;
                                m0[0] = m1[0] = m2[0] = 100;
                                m0[1] = m1[1] = m2[1] = 0;
                                object_animatorReset();
                                for (uint32_t i = 0; i < 7u; i++)
                                    object_animatorTick();
                                check((m0[0] | (m0[1] << 8)) == 100u &&
                                          (m1[0] | (m1[1] << 8)) == 99u &&
                                          (m2[0] | (m2[1] << 8)) == 98u &&
                                          (g[4] & 0x10u) != 0u,
                                      "1000:D6B2: the top member takes -2, the "
                                      "owner 0");
                                /* 1000:D813: the state byte keeps the 0xF0 mask -
                             * the 0x80 the 0xFF clear leaves is the sentinel
                             * the original's backward walk stops on */
                                check(
                                    (g[4] & 0x80u) != 0u,
                                    "1000:D813: the state byte keeps the 0x80");
                                g[0] = 0xFF;
                                g[1] = 0xFF; /* free again */
                            }
                        }
                        /* type 6's row has no handler for command 0, hook 0 and the
                     * tail 1000:E860 (the step off) */
                        a1[8] = 6;
                        a1[9] = 0;
                        check(object_commandRun(a1, 0x01u) == OBJ_CMD_TAIL,
                              "a row with only a ported tail reports the tail");
                        /* type 15's tail is 1000:DC1F, now ported: the command
                     * runs its handler and reports the tail */
                        a1[8] = 15;
                        a1[9] = 0;
                        check(
                            object_commandRun(a1, 0x01u) == OBJ_CMD_TAIL ||
                                object_commandRun(a1, 0x01u) == OBJ_CMD_HANDLER,
                            "type 15's command now reaches its tail (1000:DC1F)");
                    }
                    /* load_terrain_b's 0xF0..0xF2 tile: the shared decoration
                 * handler (1000:556B) appends {x, y, frame, +8 = frame} with
                 * behaviour 1 (frame 0xC1 / 0xC5 / 0xC9). */
                    {
                        static uint8_t m[640];
                        uint32_t n;
                        uint8_t *r;

                        memset(m, 0, sizeof m);
                        m[2u * 32u + 3u] = 0xF0u; /* row 2, col 3 */
                        m[5u * 32u + 7u] = 0xF2u; /* row 5, col 7 */
                        object_init();
                        n = object_loadTerrainObjects(m);
                        check(n == 10u, "the decorations append two records");
                        r = object_slot(9);
                        check(r != NULL && (r[0] | (r[1] << 8)) == 48u &&
                                  (r[2] | (r[3] << 8)) == 32u &&
                                  (r[4] | (r[5] << 8)) == 0x00C1u &&
                                  (r[8] | (r[9] << 8)) == 0x00C1u,
                              "0xF0 appends frame 0xC1 at (col*16, row*16)");
                        r = object_slot(10);
                        check(r != NULL && (r[0] | (r[1] << 8)) == 112u &&
                                  (r[4] | (r[5] << 8)) == 0x00C9u,
                              "0xF2 appends frame 0xC9");
                    }
                    /* load_terrain_b's 0xA9 tile: 1000:51F9 appends the five
                 * records from the 0x1EFE offset table at
                 * (col*16 + dx, row*16 + 1 + dy), frames 0x33..0x37. */
                    {
                        static uint8_t m[640];

                        memset(m, 0, sizeof m);
                        m[3u * 32u + 2u] = 0xA9u; /* row 3, col 2 */
                        object_init();
                        check(object_loadTerrainObjects(m) == 13u,
                              "0xA9 appends its five records");
                        check(
                            (object_slot(9)[0] | (object_slot(9)[1] << 8)) ==
                                    48u &&
                                (object_slot(9)[2] |
                                 (object_slot(9)[3] << 8)) == 0x44u &&
                                (object_slot(9)[4] |
                                 (object_slot(9)[5] << 8)) == 0x33u,
                            "the first record is (col*16+0x10, row*16+0x14) frame 0x33");
                        check((object_slot(13)[4] |
                               (object_slot(13)[5] << 8)) == 0x37u,
                              "the fifth record is frame 0x37");
                        /* the handler's SI is one past the cell: live level 16 has
                     * 0x33 C2 C9 at cols 14..16 (2026-11-02) */
                        check(
                            m[4u * 32u + 2u] == 0x00u &&
                                m[4u * 32u + 3u] == 0xC2u &&
                                m[4u * 32u + 4u] == 0xC9u,
                            "0xA9 marks the next two cells a row below as 0xC2/0xC9");
                        /* 0xB7: two appended records, the only computed class
                     * (+6 = (count-6)<<8) and frame 0x116 / 0x11A */
                        {
                            static uint8_t m3[640], m4[640];

                            memset(m3, 0, sizeof m3);
                            m3[2u * 32u + 2u] = 0xB7u;
                            object_init();
                            check(object_loadTerrainObjects(m3) == 10u,
                                  "0xB7 appends two records");
                            check(
                                (object_slot(9)[4] |
                                 (object_slot(9)[5] << 8)) == 0x0116u &&
                                    (object_slot(9)[6] |
                                     (object_slot(9)[7] << 8)) == 0x0200u &&
                                    (object_slot(9)[0xA] |
                                     (object_slot(9)[0xB] << 8)) == 2u,
                                "0xB7: frame 0x116, class (count-6)<<8, +0xA = 2");
                            check(
                                (object_slot(10)[4] |
                                 (object_slot(10)[5] << 8)) == 0u &&
                                    object_slotClass(1u) == 8u &&
                                    object_slotClass(2u) == 0x85u,
                                "0xB7: behaviour 8 then an empty record with 0x85");
                            /* a 0xB6 within four columns either side: frame 0x11A */
                            memset(m4, 0, sizeof m4);
                            m4[2u * 32u + 5u] = 0xB7u;
                            m4[2u * 32u + 2u] = 0xB6u;
                            object_init();
                            (void)object_loadTerrainObjects(m4);
                            check(
                                (object_slot(10)[4] |
                                 (object_slot(10)[5] << 8)) == 0x011Au &&
                                    (object_slot(10)[0xA] |
                                     (object_slot(10)[0xB] << 8)) == 3u,
                                "0xB7 beside a 0xB6: frame 0x11A and +0xA = 3");
                            /* 0xBB / 0xC1: two more appends (0x26 / 0x100) */
                            memset(m4, 0, sizeof m4);
                            m4[1u * 32u + 1u] = 0xBBu;
                            m4[1u * 32u + 2u] = 0xC1u;
                            object_init();
                            check(
                                object_loadTerrainObjects(m4) == 10u &&
                                    (object_slot(9)[4] |
                                     (object_slot(9)[5] << 8)) == 0x0026u &&
                                    (object_slot(10)[4] |
                                     (object_slot(10)[5] << 8)) == 0x0100u &&
                                    (object_slot(10)[2] |
                                     (object_slot(10)[3] << 8)) == 0u,
                                "0xBB appends 0x26, 0xC1 appends 0x100 at y-0x10");
                            /* 0xBE + 0xBF: the join writes the 0xBF tile's x into the
                         * record 0xBE made (+0xC) */
                            memset(m4, 0, sizeof m4);
                            m4[1u * 32u + 1u] = 0xBEu;
                            m4[1u * 32u + 4u] = 0xBFu;
                            object_init();
                            check(
                                object_loadTerrainObjects(m4) == 9u &&
                                    (object_slot(9)[4] |
                                     (object_slot(9)[5] << 8)) == 0x0082u &&
                                    (object_slot(9)[0xC] |
                                     (object_slot(9)[0xD] << 8)) == 64u,
                                "0xBF joins into 0xBE's record (+0xC = its x)");
                        }
                        /* 0xAC/0xAD/0xAE: one record each, then 1000:5639 snaps its
                     * y to the terrain step at (x+8, y+0x10) */
                        {
                            static uint8_t m2[640];

                            memset(m2, 0, sizeof m2);
                            m2[1u * 32u + 1u] = 0xACu;
                            m2[1u * 32u + 2u] = 0xADu;
                            m2[1u * 32u + 3u] = 0xAEu;
                            object_init();
                            check(
                                object_loadTerrainObjects(m2) == 11u,
                                "the three style tiles append one record each");
                            check((object_slot(9)[4] |
                                   (object_slot(9)[5] << 8)) == 0x96u &&
                                      (object_slot(10)[4] |
                                       (object_slot(10)[5] << 8)) == 0x9Cu &&
                                      (object_slot(11)[4] |
                                       (object_slot(11)[5] << 8)) == 0x38u,
                                  "campfire 0x96, torch 0x9C, stone 0x38");
                            /* level 1 is style 1, so 1000:5662's +3 nudge must not
                         * apply even with a marked neighbour below-left */
                            check((object_slot(9)[2] |
                                   (object_slot(9)[3] << 8)) !=
                                          (uint16_t)(16u + 3u) ||
                                      level_style() == 6u,
                                  "the +3 nudge is gated on the style");
                            check((object_slot(9)[8] |
                                   (object_slot(9)[9] << 8)) == 0xFFFFu &&
                                      (object_slot(11)[8] |
                                       (object_slot(11)[9] << 8)) == 0u &&
                                      (object_slot(11)[0xC] |
                                       (object_slot(11)[0xD] << 8)) == 0xFFFFu,
                                  "the style fields (+8 / +0xC) are per tile");
                        }
                    }
                    /* The +0xA hook, 1000:D8BD (the terrain settle), on a special
                 * tile: the settle returns before writing anything, so the only
                 * change is the idle handler's, and the tail still runs. */
                    {
                        const uint8_t *map = level_map();
                        uint32_t sp0 = level_specialTile(0u),
                                 sp1 = level_specialTile(1u);
                        uint8_t *a1;
                        int32_t found = 0, col = 0, row = 0;

                        for (uint32_t r = 0; r < 640u && !found; r++)
                            if (map[r] == sp0 || map[r] == sp1) {
                                found = 1;
                                row = (int32_t)(r / 32u);
                                col = (int32_t)(r % 32u);
                            }
                        check(found != 0, "a special tile for the settle test");
                        object_init();
                        a1 = object_slot(0);
                        a1[0] = (uint8_t)(16 * col - 8);
                        a1[1] = 0;
                        a1[2] = (uint8_t)(16 * row - 0x18);
                        a1[3] = 0;
                        a1[4] = 0;
                        a1[5] = 0;
                        a1[6] = 0;
                        a1[7] = 0;
                        a1[8] = 2;
                        a1[9] = 0;
                        a1[0xA] = 0;
                        a1[0xB] = 0;
                        object_setActionEntity(0u);
                        /* command 6 (input 0x04) -> 1000:E72A leaves the type at 2
                     * (frame 0x10), so the hook runs; the feet are on a special
                     * tile, which stops the settle before it writes anything */
                        check(
                            object_commandRun(a1, 0x04u) == OBJ_CMD_TAIL,
                            "the settle hook runs and the tail still follows");
                        check(
                            (a1[2] | (a1[3] << 8)) ==
                                    (uint16_t)(16 * row - 0x18) &&
                                (a1[4] | (a1[5] << 8)) == 0x0010u &&
                                (a1[8] | (a1[9] << 8)) == 2u,
                            "a special tile stops the settle before it writes");
                    }
                }
                /* the four placed humans again for the second-record check */
                object_init();
                for (uint32_t i = 0; i < 4u; i++) {
                    static const uint16_t pos[4][2] = {
                        { 48, 25 }, { 208, 25 }, { 256, 137 }, { 465, 265 }
                    };
                    rec = object_slot(i);
                    rec[0] = (uint8_t)pos[i][0];
                    rec[1] = (uint8_t)(pos[i][0] >> 8);
                    rec[2] = (uint8_t)pos[i][1];
                    rec[3] = (uint8_t)(pos[i][1] >> 8);
                    rec[4] = 0x10;
                    rec[6] = 0x00;
                    rec[8] = 0x02;
                }
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
            }
            /* and the same law at the second human (208,25) */
            {
                uint32_t wrong = 0;
                for (uint32_t row = 0; row < 24u; row++)
                    for (uint32_t col = 0; col < 16u; col++) {
                        uint8_t v = bank[0x4800u + (col & 3u) * 0x60u +
                                         row * 4u + (col >> 2)];
                        if (page[(25u + row) * 320u + 208u + col] != v) wrong++;
                    }
                check(wrong == 0u, "the second placed record is drawn too");
            }
            /* the class overlay (1000:653C/1000:6570): record 0 carries class id
             * 5, whose slot is [0x3E5E + 5] (the listing's 0x3E5F + class-1);
             * behaviour class 5 maps to column 1 (0x3566[5]); frame 1's row is
             * 2 (0x3570[1]), so the entry is 0x368A[3*2+1] = 2 = the
             * {4, 8, 0x5A, 0x69} record: the class's placed record (slot 8+5)
             * is moved to (x+4, y+8), given frame 0x5A and drawn. */
            {
                uint8_t *host = object_slot(0u);
                uint8_t *carried = object_slot(13u);

                rec = host;
                rec[4] = 0x01;
                rec[5] = 0x00; /* frame 1 */
                rec[7] = 5u; /* the class slot id */
                object_setSlotClass(5u, 5u); /* behaviour class 5 */
                memset(carried, 0, 0x12u);
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
                check((carried[0] | (carried[1] << 8)) == 52u &&
                          (carried[2] | (carried[3] << 8)) == 33u &&
                          (carried[4] | (carried[5] << 8)) == 0x5Au,
                      "the class overlay moves and frames the carried record");
                rec[7] = 0u;
                object_setSlotClass(5u, 0u);
            }
            /* D-51: the pickup's taken mark (the slot byte's bit 7) makes the
             * record walk skip the record (1000:6450 no-class / 1000:6467
             * class path); only a carrier's overlay draws it.  The 0xAB spear
             * is live index 9 -> class slot 1. */
            {
                static uint8_t m5[640];
                uint8_t *sp;
                uint32_t ink;

                memset(m5, 0, sizeof m5);
                m5[2u * 32u + 2u] = 0xABu; /* the spear tile */
                object_init();
                check(object_loadTerrainObjects(m5) == 9u,
                      "the 0xAB spear for the taken-record walk");
                sp = object_slot(9);
                sp[0] = 10;
                sp[1] = 0;
                sp[2] = 10;
                sp[3] = 0;
                check((sp[4] | (sp[5] << 8)) == 0x0060u &&
                          object_slotClass(1u) == 5u,
                      "the spear sits in live slot 1");
                object_setSlotClass(1u, 0x85u); /* the pickup's mark */
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
                ink = 0;
                for (uint32_t y = 10; y < 26; y++)
                    for (uint32_t x = 10; x < 26; x++)
                        if (page[y * 320u + x]) ink++;
                check(ink == 0u,
                      "1000:6450: a taken record is not drawn by the walk");
                /* control: with the mark clear the same record draws */
                object_setSlotClass(1u, 5u);
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
                ink = 0;
                for (uint32_t y = 10; y < 26; y++)
                    for (uint32_t x = 10; x < 26; x++)
                        if (page[y * 320u + x]) ink++;
                check(ink != 0u, "the same record draws while it is not taken");
            }
            /* the mover's record pass (1000:5A2F): a candidate sitting exactly
             * on the far edge of both windows is collected (the listing's
             * ADD/JNS is >=), the ACTING record's class byte takes 0x80 and
             * the candidate's +7 gets the acting record's ordinal. */
            {
                static uint8_t m6[640];
                uint8_t *spear, *cand;
                uint16_t lifeBefore;

                memset(m6, 0, sizeof m6);
                m6[4u * 32u + 4u] = 0xABu; /* the mover, at (64,64) */
                m6[6u * 32u + 6u] = 0xB7u; /* the behaviour-8 candidate */
                object_init();
                (void)object_loadTerrainObjects(m6);
                spear = object_slot(9u); /* live ordinal 1 */
                cand = object_slot(10u); /* live ordinal 2 */
                check(spear && cand && object_slotClass(1u) == 5u &&
                          object_slotClass(2u) == 8u,
                      "the mover and the behaviour-8 candidate");
                /* frame 0x60 selects (dx,dy) = (+8,+15); the window's far
                 * edges are x - 0x10 and y - 0x1A */
                cand[0] = (uint8_t)(64 + 8 - 0x10);
                cand[1] = 0u;
                cand[2] = (uint8_t)(64 + 15 - 0x1A);
                cand[3] = 0u;
                cand[6] &= 0x7Fu;
                cand[7] = 0u;
                lifeBefore = (uint16_t)(spear[0xE] | (spear[0xF] << 8));
                object_updateList();
                check(cand[7] == 1u,
                      "the candidate links to the acting ordinal");
                check(object_slotClass(1u) == 0x85u,
                      "the acting record's class byte takes 0x80");
                check((spear[6] | (spear[7] << 8)) == 0u,
                      "the acting record's +6 word is cleared");
                check((uint16_t)(spear[0xE] | (spear[0xF] << 8)) == lifeBefore,
                      "the acting record's +0xE is not cleared");
            }
            object_publishBank(NULL, 0u);
            free(bank);
        }
        object_init();
    } else printf("skip: the level data is not on the disc\n");

    /* 7. draw_status_indicator (1000:1FEF): a present volume returns at
     * once (no screen); a missing one draws the prompt and waits */
    check(res_volumePresent(1u), "disc 1 is present");
    check(level_diskStatusBegin(1u) == 0, "a present disc skips the prompt");
    {
        check(!res_volumePresent(3u), "disc 3 is the missing-volume case");
        check(level_diskStatusBegin(3u) == 1,
              "a missing disc draws the prompt");
        check(palette_fadeActive(), "the prompt fades its palette in");
        for (uint32_t t = 0; t < 80u && palette_fadeActive(); t++)
            palette_tick();
        check(!palette_fadeActive(), "the prompt fade completes");
        {
            InputState k;
            memset(&k, 0, sizeof k);
            k.lastKey = 1;
            check(level_diskStatusTick(&k) == 1,
                  "the prompt waits while the disc is still missing");
        }
        level_diskStatusReset();
        check(res_volumePresent(0u), "disc 0 is present");
    }

    /* 7b. the post-level cutscene decision (entry 14E6/14FF) and the
     * ending index (150C/1512), against the real header bytes */
    check(gameflow_postLevelScene(1, 0u) == 0u,
          "a won level with intro 0 plays scene 0");
    check(gameflow_postLevelScene(1, 0xFFu) == 0xFFFFu,
          "intro 0xFF = no newspaper");
    check(gameflow_postLevelScene(0, 0u) == 0xFFFFu,
          "a lost level never plays its newspaper");
    check(gameflow_endingScene(1) == 7u && gameflow_endingScene(0) == 6u,
          "ending scene 7 COMPLETE / 6 lost");
    if (level_load(0) == 0)
        check(level_data()->introIndex == 0u, "level 1's header intro is 0");
    if (level_load(1) == 0)
        check(level_data()->introIndex == 0xFFu,
              "level 2's header intro is 0xFF (no newspaper)");
    if (level_load(5) == 0)
        check(level_data()->introIndex == 4u, "level 6's header intro is 4");

    /* 7b2. D-55: the briefing's "THE CODE FOR THIS LEVEL IS....." value is
     * the level record's author (load_level 1000:30BC copies it to 0x1189),
     * not a fixed string. */
    if (level_load(1) == 0)
        check(strcmp(gameflow_partyTribe(), "ANDIE PANDY") == 0,
              "level 2's briefing code is its author");
    if (level_load(3) == 0)
        check(strcmp(gameflow_partyTribe(), "CARLOS") == 0,
              "level 4's briefing code follows the level");

    /* 6c3. D-64/D-65: a successful pickup sets [0x5B56] = [0x5B8A + class]
     * (class 5 -> 2, the bar's 4-icon spear group), and op 6 with the class
     * set runs the class path 1000:9D5D: the carried record is thrown at the
     * acting record's feet and the bar returns to group 1. */
    object_init(); /* fresh array: 6c2 marked the spear */
    if (level_load(0) == 0) {
        uint8_t *actor = object_slot(0), *spear = object_slot(9);

        /* a level start leaves [0x1988] = 1, so prog[1] = 1 and the press
         * reads prog[2] = op 2; earlier blocks may have left a stale cursor */
        object_actionSetCursor(1u);
        check(actor != NULL && spear != NULL &&
                  (spear[4] | (spear[5] << 8)) == 0x0060u,
              "the spear is live at index 9");
        object_setActionEntity(0u);
        actor[0] = (uint8_t)(448u - 8u);
        actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u;
        actor[3] = 0;
        actor[4] = 0x10;
        actor[6] = 0;
        actor[8] = 2;
        actor[9] = 0;
        check(object_actionProgram() == 1u && statusbar_actionGroup() == 1u,
              "the bar starts on group 1");
        object_actionRun();
        check(actor[7] == 1u && object_slotClass(1u) == 0x85u,
              "the pickup takes the spear");
        check(object_actionProgram() == 2u && statusbar_actionGroup() == 2u,
              "the pickup moves the bar to the spear's group 2");
        check(statusbar_actionCount(1u) == 2u &&
                  statusbar_actionCount(2u) == 4u,
              "group 2 shows four icons where group 1 shows two");
        /* program 2's opcode walk is prog[2..5] = 3,4,5,6 - cursor 4 is op 6 */
        object_actionSetCursor(4u);
        object_actionRun();
        check(object_actionProgram() == 1u && statusbar_actionGroup() == 1u,
              "dropping it puts the bar back on group 1");
        check(actor[7] == 0u && object_slotClass(1u) == 0x05u,
              "the class and the taken mark are cleared");
        /* 1000:E361 aligns the probe row: row 48 + the 0xAB page byte's bit 7
         * leaves BX = 48, so y = 48 - 0xF = 33 (the old 41 was unaligned). */
        check((spear[0] | (spear[1] << 8)) == 440u &&
                  (spear[2] | (spear[3] << 8)) == 33u &&
                  (spear[4] | (spear[5] << 8)) == 0x0060u &&
                  (spear[8] | (spear[9] << 8)) == 0xFFFFu,
              "the spear is thrown to the actor's feet, frame 0x60");
        object_actionSetCursor(1u);
    }

    /* 6c9. D-68: the Z-only release.  [0x42F0] is never set in the image, so
     * the 0xE3F9 ACTION edge is dead and a fresh press reaches the type row's
     * fire half (row[0x19] = 0x30 = 0xEFD4, the turn), which latches [0x6F1D];
     * the next (masked) frame's idle form (0xEFD7) then runs 0xEFE4 and the
     * record leaves the pose (type += 2, frame += 1). */
    object_init();
    if (level_load(1) == 0) { /* level 2: positional goal */
        uint32_t ord = 0, i;
        uint8_t *actor = object_slot(0), *it = NULL;

        for (i = 1; i <= object_liveCount() - 8u; i++)
            if (object_slotClass(i) == 5u) {
                ord = i;
                break;
            }
        check(ord != 0u, "level 2 has a spear for the release test");
        if (ord) {
            it = object_slot(8u + ord);
            object_setActionEntity(0u);
            object_actionSetCursor(1u);
            object_setActionLatched(0);
            actor[0] = (uint8_t)((it[0] | (it[1] << 8)) - 8u);
            actor[1] = (uint8_t)(((it[0] | (it[1] << 8)) - 8u) >> 8);
            actor[2] = it[2];
            actor[3] = it[3];
            actor[4] = 0x10;
            actor[6] = 0;
            actor[8] = 2;
            actor[9] = 0;
            object_actionRun(); /* the pickup */
            object_actionSetCursor(2u);
            actor[8] = 2;
            actor[9] = 0;
            actor[4] = 0x10;
            object_actionRun(); /* op 4: the pose */
            check((uint32_t)(actor[4] | (actor[5] << 8)) == 0x00BEu,
                  "the throw pose is up before the release");
            /* the press frame: the fire half selects the turn; pause_toggle
             * is gated on [0x6F1D], so a posed record keeps [0x13A4] clear
             * (live trace: act 00, joy 80 while Z is held) */
            object_setActionLatched(0);
            object_commandRun(actor, 0x80u);
            check((uint32_t)(actor[0xEu] | (actor[0xFu] << 8)) == 4u,
                  "the fire press starts the aim (+0xE = 4)");
            /* the next frame: the fire is masked, the idle form releases */
            object_commandRun(actor, 0x00u);
            /* 0xEFE4 bumps type +2 / frame +1 and then runs the fall machine,
             * so the immediate result is the landing state (type 0/1), never
             * the pose again. */
            check(!((uint32_t)(actor[4] | (actor[5] << 8)) == 0x00BEu &&
                    (uint32_t)(actor[8] | (actor[9] << 8)) == 0x0018u),
                  "the release leaves the pose (0xEFE4)");
        }
    }

    /* 6c8. D-70: the record command index is `row[1 + cmd]` with no upper
     * bound (1000:E4A7), and DS:0x6908[0] = 0x48 makes "no direction" cmd 8.
     * A type-0x18 (throw pose) record therefore selects row[9] = 0x31
     * (0xEFD7, the idle form) with no input and the fire half's row[0x19] =
     * 0x30 (0xEFD4, the turn) with the fire bit. */
    object_init();
    if (level_load(0) == 0) {
        uint8_t *actor = object_slot(0);
        ObjCommand c;

        object_setActionEntity(0u);
        actor[6] = 0;
        actor[7] = 0;
        actor[8] = 0x18;
        actor[9] = 0; /* the throw pose's type */
        actor[4] = 0xBE;
        actor[5] = 0;
        c = object_commandSelect(actor, 0x00u);
        check(c.handler == 0xEFD7u,
              "the no-input command selects the pose row's row[9] (0xEFD7)");
        c = object_commandSelect(actor, 0x80u);
        check(
            c.handler == 0xEFD4u,
            "the fire-only command selects the fire half's row[0x19] (0xEFD4)");
    }

    /* 6c7. D-68: the pending-event 4 charge loop.  With the throw pose up,
     * holding a direction walks +0xE (0xEE07) and draws the charge bar
     * (0x3747) in the bar band; the ACTION press ends the charge (0x373B) and
     * the release (0xEFD7 -> 0xEFE4) performs the jump/throw. */
    object_init();
    if (level_load(1) == 0) { /* level 2: positional goal */
        uint32_t ord = 0, split = 0, i;
        uint8_t *actor = object_slot(0);
        uint8_t *it = NULL;

        for (i = 1; i <= object_liveCount() - 8u; i++)
            if (object_slotClass(i) == 5u) {
                ord = i;
                break;
            }
        check(ord != 0u, "level 2 has a spear");
        if (ord) {
            it = object_slot(8u + ord);
            object_setActionEntity(0u);
            object_actionSetCursor(1u);
            actor[0] = (uint8_t)((it[0] | (it[1] << 8)) - 8u);
            actor[1] = (uint8_t)(((it[0] | (it[1] << 8)) - 8u) >> 8);
            actor[2] = it[2];
            actor[3] = it[3];
            actor[4] = 0x10;
            actor[6] = 0;
            actor[8] = 2;
            actor[9] = 0;
            object_actionRun(); /* pickup */
            object_actionSetCursor(2u);
            actor[8] = 2;
            actor[9] = 0;
            actor[4] = 0x10;
            object_actionRun(); /* op 4: the pose */
            check((uint32_t)(actor[4] | (actor[5] << 8)) == 0x00BEu &&
                      (uint32_t)(actor[8] | (actor[9] << 8)) == 0x0018u,
                  "the throw pose is up (frame 0xBE, type 0x18)");
            /* earlier blocks may have left the ACTION latch or the [0x6F1D]
             * sequence latch set: SPACE home-states the pose (clearing both)
             * and the op re-poses, so the turn starts from a known state */
            object_setActionLatched(0);
            object_spaceAction();
            object_setActionLatched(
                0); /* 0xEE01 sets it; a release clears it */
            object_actionSetCursor(2u);
            actor[8] = 2;
            actor[9] = 0;
            actor[4] = 0x10;
            object_actionRun();
            for (i = 0; i < 60u; i++) statusbar_tick(); /* split -> 152 */
            split = statusbar_split();
            object_setActionLatched(0); /* poll_input 0x85A9 */
            object_commandRun(actor, 0x02u); /* DOWN: the turn */
            check((uint32_t)(actor[0xEu] | (actor[0xFu] << 8)) == 4u,
                  "the turn starts the charge (+0xE = 4)");
            object_pendingRun(); /* event 4: enter the loop */
            check(object_chargeTick() == 1, "the charge loop runs");
            {
                static const uint8_t shade[10] = {
                    0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE3, 0xE2, 0xE1, 0xE0, 0xDF
                };
                uint32_t v = 4u, ax = 0x60u + (v >> 1u);
                uint32_t x = 4u * (ax >> 2) + (ax & 3u), bad = 0, r;

                for (r = 0; r < 10u; r++)
                    if (video_page(0)[(split + 15u + r) * 320u + x] != shade[r])
                        bad++;
                check(bad == 0u, "the charge bar is drawn at +0xE = 4");
            }
            object_commandRun(actor, 0x02u); /* another turn frame */
            object_pendingRun();
            check(object_chargeTick() == 1, "the loop keeps running");
            {
                uint32_t v = 8u, ax = 0x60u + (v >> 1u);
                uint32_t x = 4u * (ax >> 2) + (ax & 3u);

                check(video_page(0)[(split + 15u) * 320u + x] == 0xDFu,
                      "the bar follows +0xE");
            }
            object_actionRun(); /* the release press */
            check(object_chargeTick() == 0, "the press ends the charge");
            {
                uint32_t f0 = actor[4] | (actor[5] << 8);
                uint32_t t0 = actor[8] | (actor[9] << 8);

                object_commandRun(actor, 0x01u); /* UP: 0xEFD7 */
                check((uint32_t)(actor[4] | (actor[5] << 8)) != f0 ||
                          (uint32_t)(actor[8] | (actor[9] << 8)) != t0,
                      "the release performs the jump/throw (0xEFE4)");
            }
        }
    }

    /* 6c5. D-66: the selected cell IS the program cursor (1000:3947 writes it,
     * 1000:3987 reads prog[cursor+1]), so the spear's four icons run four
     * different ops: 3 (pose), 4 (frame 0xBB/0xBE + type 0x17/0x18), 5 (the
     * frame step), 6 (the collision pass). */
    object_init();
    if (level_load(0) == 0) {
        uint8_t *actor = object_slot(0);

        object_setActionEntity(0u);
        object_actionSetCursor(1u);
        actor[0] = (uint8_t)(448u - 8u);
        actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u;
        actor[3] = 0;
        actor[4] = 0x10;
        actor[6] = 0;
        actor[8] = 2;
        actor[9] = 0;
        object_actionRun(); /* the pickup: op 2 */
        /* the six groups' cell bytes persist across levels (they are the DS
         * image's), so pin this one before the sequence */
        object_actionSetCursor(1u);
        check(actor[7] == 1u && statusbar_actionCell(2u) == 1u,
              "the pickup leaves the spear group on cell 1");
        check(object_programHeader(2u) != NULL &&
                  object_programHeader(2u)[2] == 3u &&
                  object_programHeader(2u)[3] == 4u &&
                  object_programHeader(2u)[4] == 5u &&
                  object_programHeader(2u)[5] == 6u,
              "group 2's icons are ops 3,4,5,6");
        object_spaceAction(); /* SPACE: cell 2 */
        check(statusbar_actionCell(2u) == 2u,
              "SPACE steps the cell to 2 (1000:3947)");
        object_actionRun();
        check((actor[4] | (actor[5] << 8)) == 0x00BEu &&
                  (actor[8] | (actor[9] << 8)) == 0x0018u,
              "cell 2 runs op 4 (frame 0xBE, type 0x18)");
        /* the posed type must be stood up before the next press (1000:392D),
         * then SPACE steps to cell 3 */
        object_spaceAction();
        object_spaceAction();
        check(statusbar_actionCell(2u) == 3u, "SPACE reaches cell 3");
        object_actionRun();
        check((actor[4] | (actor[5] << 8)) == 0x0112u &&
                  (actor[8] | (actor[9] << 8)) == 0x0020u,
              "cell 3 runs op 5 (frame 0x112, type 0x20)");
        object_actionSetCursor(1u);
    }

    /* 6c6. D-67: the overlay frame table runs past 0x7F, so the carried record
     * is drawn in the pose frames.  A class record at frame 0xBB (row 0x1F)
     * with class slot 5 (col 1) resolves entry 0x52 = {06,09,60,00}: the
     * carried spear's frame 0x60 at (x+6, y+9); frame 0x8C (row 0x17) resolves
     * entry 0x16 = {FE,FB,5B,79}: frame 0x5B at (x-2, y-5), ybias 4. */
    object_init();
    if (level_load(0) == 0) {
        uint32_t bankSize = 0;
        uint8_t *bank = res_loadChunk("APL\\SPRITES.DAT", &bankSize, NULL);
        uint8_t *actor = object_slot(0);
        static uint8_t page[320u * 200u];

        object_setActionEntity(0u);
        object_actionSetCursor(1u);
        actor[0] = (uint8_t)(448u - 8u);
        actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u;
        actor[3] = 0;
        actor[4] = 0x10;
        actor[6] = 0;
        actor[8] = 2;
        actor[9] = 0;
        object_actionRun(); /* the pickup: class 1, slot 0x85 */
        check(bank != NULL && bankSize > 0x10A0u + 0x40u,
              "SPRITES.DAT loads for the overlay test");
        if (bank) {
            uint32_t ink = 0, wrong = 0, row, col;

            object_publishBank(bank, bankSize);
            /* the marker is at 440, off the 320-wide page: move the carrier and
             * the carried record somewhere visible (the overlay re-derives the
             * carried x/y from the carrier anyway) */
            actor[0] = 100u;
            actor[1] = 0;
            actor[2] = 25u;
            actor[3] = 0;
            object_slot(9)[0] = 108u;
            object_slot(9)[1] = 0;
            actor[4] = 0xBBu;
            actor[5] = 0; /* the throw pose */
            actor[8] = 1;
            actor[9] = 0;
            memset(page, 0, sizeof page);
            object_render(page, 0, 0);
            /* frame 0x60 = {4256, 16, 0, 1}: the mode-1 law, block h*4 = 0x40 */
            for (row = 0; row < 16u; row++)
                for (col = 0; col < 16u; col++) {
                    uint8_t v = bank[0x10A0u + (col & 3u) * 0x40u + row * 4u +
                                     (col >> 2)];

                    if (!v) continue;
                    ink++;
                    if (page[(34u + row) * 320u + 106u + col] != v) wrong++;
                }
            check(ink > 40u && wrong == 0u,
                  "the carried spear is drawn at (x+6,y+9) in the throw pose");
            /* frame 0x8C (row 0x17) resolves entry 0x16 = {FE,FB,5B,79}: the
             * carried record is moved to (x-2, y-5) with frame 0x5B (the
             * entry's trailing 0x79 sprite is drawn over part of it, so the
             * pixel law is only checked on the throw pose above) */
            actor[4] = 0x8Cu;
            actor[5] = 0;
            memset(page, 0, sizeof page);
            object_render(page, 0, 0);
            check((object_slot(9)[0] | (object_slot(9)[1] << 8)) == 98u &&
                      (object_slot(9)[2] | (object_slot(9)[3] << 8)) == 20u &&
                      (object_slot(9)[4] | (object_slot(9)[5] << 8)) == 0x005Bu,
                  "the aim pose carries the spear at (x-2,y-5), frame 0x5B");
            object_publishBank(NULL, 0u);
        }
        free(bank);
    }

    /* 6c4. D-65's behaviour-4 half (1000:9D94): level 13's 0xAE tile at
     * (48,16) is a behaviour-4 record whose +0xE is 0xFFFF (1000:52EB), the
     * gate the give/hold handler 0x9CF0 tests with bit 15.  A give attaches
     * it (the actor turns type 0x0F, program 6 = 0x5B8A[4]), and op 6 then
     * takes the class path: 0x9D94 puts the actor back at the record's place. */
    if (level_load(12) == 0) {
        uint32_t o, ordinal = 0;

        for (o = 1; o <= object_liveCount() - 8u; o++)
            if (object_slotClass(o) == 4u) {
                ordinal = o;
                break;
            }
        check(ordinal != 0u, "level 13's 0xAE record takes behaviour 4");
        if (ordinal) {
            uint8_t *it = object_slot(8u + ordinal), *actor = object_slot(0);
            uint32_t ix = it[0] | (it[1] << 8), iy = it[2] | (it[3] << 8);

            check((it[0xEu] | (it[0xFu] << 8)) == 0xFFFFu,
                  "the 0xAE record carries +0xE = 0xFFFF (1000:52EB)");
            object_setActionEntity(0u);
            object_actionSetCursor(1u);
            actor[0] = (uint8_t)(ix - 8u);
            actor[1] = (uint8_t)((ix - 8u) >> 8);
            actor[2] = it[2];
            actor[3] = it[3];
            actor[4] = 0x10;
            actor[6] = 0;
            actor[8] = 2;
            actor[9] = 0;
            object_actionRun(); /* the give / hold */
            check(actor[7] == ordinal && object_slotClass(ordinal) == 0x84u &&
                      (actor[8] | (actor[9] << 8)) == 0x000Fu,
                  "the give attaches the record and makes the actor type 0x0F");
            check(object_actionProgram() == 6u && statusbar_actionGroup() == 6u,
                  "the give moves the bar to behaviour 4's group 6");
            object_actionRun(); /* program 6: op 6 */
            check(actor[7] == 0u && object_slotClass(ordinal) == 0x04u,
                  "the class path clears the class and the mark");
            check((uint32_t)(actor[2] | (actor[3] << 8)) == iy - 8u,
                  "the class path puts the actor at the record's place");
            check(object_actionProgram() == 1u && statusbar_actionGroup() == 1u,
                  "the drop puts the bar back on group 1");
        }
    }

    /* 7b5. D-62: render_objects' tail (1000:5F54) blits the BD C block - two
     * 32x24 mode-2 sprites at (x, y) and (x+0x20, y) - for each 0x32B9 entry.
     * Level 5's single 0xA9 is at (352, 257), so with the view at (192,128)
     * the halves land at (160,129) and (192,129). */
    if (level_load(4) == 0) {
        const uint8_t *cb = level_cBlockForProbe();
        static uint8_t flat[320u * 200u];

        check(cb != NULL, "level 5 has the 0xA9 background art");
        if (cb) {
            uint32_t ink = 0, bad = 0, s, row, col, i;

            for (i = 0; i < 0x600u; i++)
                if (cb[i]) ink++;
            check(ink > 300u, "the C block carries the branch art");
            /* level_setup_screen's add_delta_nonzero(0x600, 0x20): every
             * non-zero C byte moves into the 0x20..0x3F art bank (BD2's raw
             * block is 0x18..0x1E). */
            {
                uint32_t out = 0;
                for (i = 0; i < 0x600u; i++)
                    if (cb[i] && (cb[i] < 0x20u || cb[i] > 0x3Fu)) out++;
                check(out == 0u, "the C block carries the +0x20 art bias");
            }
            /* the draw law, on a blank page at the marker */
            memset(flat, 0, sizeof flat);
            level_drawBgSprites(flat, 352, 257);
            for (s = 0; s < 2u; s++)
                for (row = 0; row < 24u; row++)
                    for (col = 0; col < 32u; col++) {
                        uint8_t want = cb[s * 0x300u + (col & 3u) * 0xC0u +
                                          row * 8u + (col >> 2)];

                        if (flat[row * 320u + s * 0x20u + col] != want) bad++;
                    }
            check(bad == 0u, "the C block blits as two 32x24 halves");
            /* and the pass is wired into the frame */
            level_setView(192u, 128u);
            level_redrawPage();
            {
                const uint8_t *page = video_page(0);
                uint32_t seen = 0, hit = 0;

                for (s = 0; s < 2u; s++)
                    for (row = 0; row < 24u; row++)
                        for (col = 0; col < 32u; col++) {
                            uint8_t v = cb[s * 0x300u + (col & 3u) * 0xC0u +
                                           row * 8u + (col >> 2)];

                            if (!v) continue;
                            seen++;
                            if (page[(129u + row) * 320u + 160u + s * 0x20u +
                                     col] == v)
                                hit++;
                        }
                check(seen == ink && hit == seen,
                      "the 0xA9 branch art is on the level 5 page");
            }
            level_setView(0u, 0u);
        }
    }

    /* 7b6. D-72/D-73: the DEFS field-G sprites.  Level 16's F0/F1/F2 tiles
     * at (320,304), (336,304), (352,304) append records with frames
     * 0xC1/0xC5/0xC9; those frame slots are written by the level loader from
     * the DEFS G blocks (3 layers x n x 0x100, live DS:0x3CAC), not by
     * apply_relocations - the image's slots are zero.  The live read proves
     * BD6's biased G block 0 is exactly the sprite at the water hole
     * (2026-11-02: linear 0x1A40A). */
    if (level_load(15) == 0) {
        static uint8_t flat[320u * 200u];
        uint32_t frames[3] = { 0xC1u, 0xC5u, 0xC9u };
        uint32_t k, found = 0, water = 0;

        for (k = 0; k < 3u; k++) {
            uint32_t off = 0, rows = 0, i;

            /* the image's own table leaves these slots blank */
            object_frameForProbe(frames[k], &off, &rows);
            if (rows != 0u) continue;
            for (i = 8u; i <= object_liveCount(); i++) {
                uint8_t *r = object_slot(i);
                if ((uint32_t)(r[4] | (r[5] << 8)) == frames[k]) {
                    found++;
                    break;
                }
            }
        }
        check(found == 3u,
              "level 16 keeps the three decoration records (frames C1/C5/C9)");
        /* the G pass draws them: the pool cells must carry the G bank
         * (0x21..0x3F), where the BG alone is 0x40..0x47 */
        level_setView(192u, 168u);
        level_redrawPage();
        {
            const uint8_t *page = video_page(0);
            uint32_t x, y;

            for (y = 136u; y < 152u; y++)
                for (x = 128u; x < 176u; x++) {
                    uint8_t v = page[y * 320u + x];
                    if (v >= 0x21u && v <= 0x3Fu) water++;
                }
        }
        memset(flat, 0, sizeof flat);
        check(water > 100u, "the DEFS G sprites draw the level 16 water hole");
    }

    /* 7b7. D-72: the A9 log drop.  The settle's 0xDF2C writes 0xFF at
     * firstRecord + 8 + 0x12*k (the tables 0x689E/0x68BE), and 1000:D5E4 then
     * falls that record +2 y per tick and hides it past y 0x138.  Live-
     * verified 2026-11-02 (log 9's +8 = 1 -> (240,276) to (240,314), +6 = 0xFF). */
    if (level_load(15) == 0) {
        uint8_t *log = NULL;
        uint32_t i, col, row;

        for (i = 8u; i <= object_liveCount(); i++) {
            uint8_t *r = object_slot(i);

            if ((uint32_t)(r[4] | (r[5] << 8)) == 0x33u) {
                log = r;
                break;
            }
        }
        check(log != NULL, "level 16's first A9 log record exists");
        /* the marker pair lands one cell right of the 0xA9 */
        for (row = 0; row < 20u; row++)
            for (col = 0; col < 32u; col++)
                if (level_map()[row * 32u + col] == 0xA9u) goto found;
    found:
        check(row < 20u && level_map()[(row + 1u) * 32u + col] == 0x33u &&
                  level_map()[(row + 1u) * 32u + col + 1u] == 0xC2u &&
                  level_map()[(row + 1u) * 32u + col + 2u] == 0xC9u,
              "the 0xA9 markers sit one cell right of the tile (live)");
        if (log) {
            uint8_t *logs[5];
            uint32_t k;

            for (k = 0; k < 5u; k++) logs[k] = log + k * 0x12u;
            /* the marker pair, as 1000:DF2C leaves it */
            check(level_map()[(row + 1u) * 32u + col + 1u] == 0xC2u &&
                      level_map()[(row + 1u) * 32u + col + 2u] == 0xC9u,
                  "the A9 marker pair is C2/C9 before a mark");
            /* the settle pokes the log the foot is on: the table offset is
             * 8 + 0x12*k, i.e. that record's +8 */
            logs[4][8] = 0xFFu; /* log 13, the rightmost */
            logs[4][9] = 0xFFu;
            object_tick(NULL);
            check((uint32_t)(logs[4][2] | (logs[4][3] << 8)) == 0x116u &&
                      !(logs[4][6] & 0x80u),
                  "a marked log falls 2 px on the next tick");
            /* live: after log 13 the pair reads C2/CA (DS:0x6697's table) */
            check(level_map()[(row + 1u) * 32u + col + 1u] == 0xC2u &&
                      level_map()[(row + 1u) * 32u + col + 2u] == 0xCAu,
                  "the marker pair encodes the marked log (C2/CA)");
            /* live: logs 11..13 marked -> C3/00 */
            logs[2][8] = logs[2][9] = 0xFFu;
            logs[3][8] = logs[3][9] = 0xFFu;
            object_tick(NULL);
            check(level_map()[(row + 1u) * 32u + col + 1u] == 0xC3u &&
                      level_map()[(row + 1u) * 32u + col + 2u] == 0x00u,
                  "the marker pair follows the marked set (C3/00)");
            /* past 0x138 the record hides itself */
            logs[4][2] = 0x38u;
            logs[4][3] = 0x01u; /* y = 0x138 */
            object_tick(NULL);
            check((uint32_t)(logs[4][2] | (logs[4][3] << 8)) == 0x13Au &&
                      (logs[4][6] & 0x80u) != 0u,
                  "past y 0x138 the log hides itself (1000:D5E4)");
        }
    }

    /* 7b8. D-77: the DEFS field-D/E and H sprites.  The A9 records' frames
     * 0x33..0x37 are blank in the image table (apply_relocations skips them)
     * but the level loader points them at field E; 0x26/0x27 at field H.
     * Live: E byte-identical at 0x1A388, H at 0x1A70A/0x1A80A. */
    if (level_load(15) == 0) {
        static uint8_t flat[320u * 200u];
        static const uint32_t frames[3] = { 0x33u, 0x26u, 0x27u };

        for (uint32_t k = 0; k < 3u; k++) {
            uint8_t *r;
            uint32_t ink = 0, i;

            object_init();
            memset(flat, 0, sizeof flat);
            r = object_slot(0);
            r[0] = 10;
            r[1] = 0;
            r[2] = 10;
            r[3] = 0;
            r[4] = (uint8_t)frames[k];
            r[5] = (uint8_t)(frames[k] >> 8);
            r[6] = 0;
            r[7] = 0;
            object_render(flat, 0, 0);
            for (i = 0; i < 320u * 200u; i++)
                if (flat[i]) ink++;
            if (k == 0u)
                check(ink > 10u,
                      "the A9 log sprite draws (field E, frame 0x33)");
            else
                check(ink > 10u, k == 1u ? "field H draws frame 0x26"
                                         : "field H draws frame 0x27");
        }
    }

    /* 7b9. D-74: the placed-record behaviour walk (1000:C295).  Level 16's
     * torch (behaviour 6) steps its frame every 4th frame; the decorations
     * (behaviour 1) reload their +0xA countdown on the 8-tick cadence. */
    if (level_load(15) == 0) {
        uint8_t *torch = NULL, *deco = NULL;
        uint32_t i, f0, seen = 0;

        for (i = 8u; i <= object_liveCount(); i++) {
            uint8_t *r = object_slot(i);

            if ((r[4] | (r[5] << 8)) == 0x9Cu) torch = r;
            if ((r[4] | (r[5] << 8)) == 0xC1u) deco = r;
        }
        check(torch != NULL && deco != NULL,
              "level 16 carries the torch and a behaviour-1 decoration");
        if (torch && deco) {
            uint32_t idx = (uint32_t)((torch - object_slot(0)) / 0x12u);

            check((uint32_t)(object_slotClass(idx - 8u) & 0x3Fu) == 6u,
                  "level 16's torch is behaviour 6");
            f0 = torch[4] | (torch[5] << 8);
            for (i = 0; i < 9u; i++) {
                object_updateList();
                object_tick(NULL);
                seen |= 1u << (uint32_t)((torch[4] | (torch[5] << 8)) - f0);
            }
            check(seen == 0x7u, "the torch walks 0x9C..0x9E (behaviour 6)");
            /* D-88: 1000:E039's settle landed the torch on its two-pixel step:
             * y 177 -> 178 (the live record's value, D-76) with +0xA/+0xE
             * cleared, so the frame walk above is not interrupted. */
            check((torch[2] | (torch[3] << 8)) == 178u &&
                      (torch[10] | (torch[11] << 8)) == 0u &&
                      (torch[14] | (torch[15] << 8)) == 0u,
                  "1000:E039 settles the torch at y = 178 (D-88)");
            {
                uint8_t *b1 = NULL;
                uint32_t j;

                for (j = 8u; j <= object_liveCount(); j++) {
                    uint8_t *q = object_slot(j);

                    if ((uint32_t)(object_slotClass(j - 8u) & 0x3Fu) == 1u) {
                        b1 = q;
                        break;
                    }
                }
                if (b1) {
                    object_updateList();
                    check((b1[10] | (b1[11] << 8)) != 0u,
                          "the behaviour-1 stepper ticks its +0xA countdown");
                }
            }
        }
    }

    /* 7b10. D-74: behaviour 5's collision passes (1000:5A2F record, 0x5B3E
     * placed).  A mover at frame 0x58 projects by (7,0) from the 0x6612
     * table; a target in that box is rewritten and the mover is consumed. */
    {
        static uint8_t m[640];
        uint8_t *mover = NULL, *slot = NULL, *rec = NULL;
        uint32_t i;

        memset(m, 0, sizeof m);
        m[10 * 32 + 10] = 0xAAu; /* a placed human  */
        m[10 * 32 + 20] = 0xABu; /* a spear: class 5 */
        m[10 * 32 + 22] = 0xB7u; /* a behaviour-8 record */
        object_init();
        (void)object_loadTerrainObjects(m);
        for (i = 8u; i <= object_liveCount(); i++) {
            uint8_t *r = object_slot(i);
            uint32_t cls = object_slotClass(i - 8u) & 0x3Fu;

            if (cls == 5u && !mover) mover = r;
            if (cls == 8u && !rec) rec = r;
        }
        slot = object_slot(0);
        check(mover && slot && rec, "the mover/slot/record fixtures spawn");
        if (mover && slot) {
            uint32_t keep = (uint32_t)((mover - object_slot(0)) / 0x12u);
            uint8_t *save = mover;

            mover[0] = 100;
            mover[1] = 0;
            mover[2] = 100;
            mover[3] = 0;
            mover[4] = 0x58;
            mover[5] = 0;
            mover[8] = 0xFF;
            mover[9] = 0xFF;
            mover[0xA] = 0;
            mover[0xB] = 0;
            mover[0xC] = 1;
            mover[0xD] = 0;
            mover[0xE] = 0;
            mover[0xF] = 0;
            slot[0] = 100;
            slot[1] = 0;
            slot[2] = 100;
            slot[3] = 0;
            slot[4] = 0x10;
            slot[6] = 0;
            slot[8] = 2;
            slot[9] = 0;
            if (rec) {
                rec[0] = 0xFF;
                rec[1] = 0xFF;
            } /* keep the record pass out */
            object_updateList();
            check((slot[4] | (slot[5] << 8)) == 0x0028u &&
                      (slot[8] | (slot[9] << 8)) == 0x0016u &&
                      (slot[0xAu] | (slot[0xBu] << 8)) == 5u &&
                      (slot[0] | (slot[1] << 8)) == 92u && save[6] == 0xFFu,
                  "a mover in reach rewrites the placed slot (1000:5B3E)");
            (void)keep;
        }
        if (mover && rec) {
            object_setSlotClassForProbe(
                (uint32_t)((mover - object_slot(0)) / 0x12u) - 8u, 5u);
            mover[6] = 0;
            mover[7] = 0;
            mover[8] = 0xFF;
            mover[9] = 0xFF;
            mover[0xA] = 0;
            mover[0xB] = 0;
            mover[0xC] = 1;
            mover[0xD] = 0;
            mover[0xE] = 0;
            mover[0xF] = 0;
            rec[0] = 100;
            rec[1] = 0;
            rec[2] = 90;
            rec[3] = 0; /* dy -10 */
            rec[4] = 0x16;
            rec[5] = 0x01;
            rec[6] = 0;
            rec[7] = 0;
            object_updateList();
            check((rec[4] | (rec[5] << 8)) == 0x0028u &&
                      (rec[8] | (rec[9] << 8)) == 3u &&
                      (rec[0xAu] | (rec[0xBu] << 8)) == 6u && mover[6] == 0xFFu,
                  "a mover in reach rewrites a behaviour-8 record (1000:5A2F)");
        }
    }

    /* 7b11. D-74: behaviour 8's state machine (1000:C57E).  A 0xB7 spawns
     * the frame 0x116/0x11A record with +0xA = 2/3; the 0xC615 animation
     * counts +8 down, reloads it and advances the frame. */
    {
        static uint8_t m[640];
        uint8_t *rec = NULL;
        uint32_t i, f0;

        memset(m, 0, sizeof m);
        m[10 * 32 + 10] = 0xB7u;
        object_init();
        (void)object_loadTerrainObjects(m);
        for (i = 8u; i <= object_liveCount(); i++) {
            uint8_t *r = object_slot(i);

            if ((object_slotClass(i - 8u) & 0x3Fu) == 8u) {
                rec = r;
                break;
            }
        }
        check(rec != NULL, "the 0xB7 behaviour-8 record spawns");
        if (rec) {
            f0 = rec[4] | (rec[5] << 8);
            object_updateList();
            check((uint32_t)(rec[4] | (rec[5] << 8)) == f0 + 1u &&
                      (rec[8] | (rec[9] << 8)) == 6u,
                  "the state-2 animation advances the frame (1000:C615)");
        }
    }

    /* 7b12. D-74: behaviour 4's projectile (1000:CEC3).  Level 13's 0xAE
     * stone is behaviour 4 with vx = vy = 0; phase 1 reads the field-A slope
     * and moves x (or starts the fall via +0xA). */
    if (level_load(12) == 0) {
        uint8_t *stone = NULL;
        uint32_t i, x0, a0;

        for (i = 8u; i <= object_liveCount(); i++) {
            if ((object_slotClass(i - 8u) & 0x3Fu) == 4u) {
                stone = object_slot(i);
                break;
            }
        }
        check(stone != NULL, "level 13's 0xAE stone is behaviour 4");
        if (stone) {
            /* phase 2: set vy = 1 and check the 0x6652 curve (-4) and the
             * vy++ step (the phase-1 slope is not guaranteed on this cell) */
            x0 = (uint32_t)(stone[0] | (stone[1] << 8));
            a0 = (uint32_t)(stone[2] | (stone[3] << 8));
            stone[0xA] = 1;
            stone[0xB] = 0;
            object_updateList();
            check((uint32_t)(stone[2] | (stone[3] << 8)) != a0 &&
                      (stone[0xA] | (stone[0xB] << 8)) == 2u,
                  "the behaviour-4 stone falls (1000:CEC3 phase 2)");
            (void)x0;
        }
    }

    /* 7b13. D-80: one ACTION latch.  pause_toggle sets it (1000:3538) and the
     * release clears it (1000:85A9); the type-row fire column (1000:EE07) then
     * walks +0xE and arms the charge loop, and the release throws (0xEFD7 ->
     * 0xEFE4).  Holds a behaviour-5 object, which is what 0xF2CF's state 1
     * test needs for op 4 to wind the human up. */
    {
        uint8_t *a;

        object_init();
        a = object_slot(0);
        object_setActionEntity(0u);
        object_actionSetProgram(2u);
        object_actionSetCursor(2u); /* program 2, cell 2 = icon 4 */
        object_setActionLatched(0);
        object_setSlotClass(1u, 5u);
        a[7] = 1u;
        a[0] = 100;
        a[1] = 0;
        a[2] = 100;
        a[3] = 0;
        a[4] = 0x10;
        a[5] = 0;
        a[6] = 0;
        a[8] = 2;
        a[9] = 0;
        a[0xA] = 0;
        a[0xB] = 0;
        a[0xC] = 0;
        a[0xD] = 0;
        a[0xE] = 0;
        a[0xF] = 0;

        object_actionRun(); /* the tap */
        check(
            (a[8] | (a[9] << 8)) == 0x0018u &&
                (a[4] | (a[5] << 8)) == 0x00BEu && object_actionLatched(),
            "op 4 winds the human up and pause_toggle latches (1000:EFA1/0x3538)");

        object_setActionLatched(0); /* the tap's release */
        check(!object_actionLatched(),
              "the release clears the latch (1000:85A9)");

        (void)object_commandRun(a, 0x80u); /* a held frame: the fire column */
        check(!object_actionLatched() &&
                  (uint32_t)(a[0xE] | (a[0xF] << 8)) == 4u &&
                  object_actionPending() == 4u,
              "the fire column charges without latching (1000:EE07)");
        object_pendingRun();
        check(object_chargeTick() == 1,
              "the charge loop draws the bar (1000:3702)");

        (void)object_commandRun(a, 0x80u); /* the next held frame */
        check((uint32_t)(a[0xE] | (a[0xF] << 8)) == 8u,
              "the charge keeps walking +0xE while ACTION is held");
        object_actionRun(); /* 1000:3530: blocked by [0x6F1D] */
        check((a[8] | (a[9] << 8)) == 0x0018u,
              "pause_toggle is blocked while the charge owns the record");

        (void)object_commandRun(a, 0x00u); /* the release */
        {
            uint32_t t = a[8] | (a[9] << 8);
            uint32_t e = a[0xE] | (a[0xF] << 8);

            /* 0xEFE4 bumps the type by 2 (0x18 -> 0x1A) and runs the fall
             * machine, which may land straight into another state; the one
             * thing the release must do is leave the wind-up - nothing else
             * in the type-0x18 no-fire column changes the type. */
            if (t == 0x0018u)
                fprintf(stderr, "  (release: type %04x +E %04x)\n", t, e);
            check(t != 0x0018u,
                  "the release leaves the wind-up (1000:EFD7 -> 0xEFE4)");
            /* D-84: the landing pair is 1000:F8D2/F8D8 - frame <- [0x4230]
             * (objSide), type <- [0x4234] (objSelXoff) - so the throw path
             * (0xF078: 8/1) must not land as frame 1/type 0. */
            check(
                (t == 0x0000u || t == 0x0001u) &&
                    ((a[4] | (a[5] << 8)) == 8u || (a[4] | (a[5] << 8)) == 0u),
                "the landing pair is frame <- [0x4230], type <- [0x4234]");
        }
    }

    /* 7b4. D-63: the sprite table runs to the 0x3E20 bound - 0x11E slots.
     * apply_relocations builds it from 0x39A8 to 0x3E20; the 0xB7 bush and
     * the type-31/32 tails use frames 0x116/0x11A, which a 0x110 table drops. */
    check(object_frameSlots() == 0x11Eu, "the sprite table holds 0x11E slots");
    {
        uint32_t sz = 0;
        uint8_t *bank = res_loadChunk("APL\\SPRITES.DAT", &sz, NULL);
        static uint8_t page[320u * 200u];

        check(bank != NULL, "SPRITES.DAT loads for the frame test");
        if (bank) {
            uint32_t k;
            /* D-69: the four tail copies of apply_relocations (0x57EF-0x580F)
             * make 0x111/0x115/0x119/0x11D carry their source slot's
             * descriptor - verified live against DS:0x39A8 on 2026-11-02. */
            {
                static const uint32_t pairs[4][2] = {
                    { 0x10Fu, 0x111u },
                    { 0x113u, 0x115u },
                    { 0x117u, 0x119u },
                    { 0x11Bu, 0x11Du },
                };
                uint32_t k, bad = 0;

                for (k = 0; k < 4u; k++) {
                    uint32_t so = 0, sr = 0, dof = 0, dr = 0;

                    object_frameForProbe(pairs[k][0], &so, &sr);
                    object_frameForProbe(pairs[k][1], &dof, &dr);
                    if (sr == 0u || dof != so || dr != sr) bad++;
                }
                check(bad == 0u,
                      "the four tail-copied frames carry their source sprite");
            }
            /* 0x11D is NOT blank: apply_relocations copies 0x11B's descriptor
             * into it (0x57EF-0x580F); 0xB8 is a genuinely empty slot. */
            static const uint32_t frames[3] = { 0x116u, 0x11Au, 0xB8u };
            static const char *whats[3] = {
                "frame 0x116 (the 0xB7 bush) draws",
                "frame 0x11A (the 0xB7 bush, near a 0xB6) draws",
                "frame 0xB8 (an empty slot) draws nothing",
            };

            for (k = 0; k < 3u; k++) {
                uint8_t *r;
                uint32_t ink = 0, i;

                object_init();
                object_publishBank(bank, sz);
                memset(page, 0, sizeof page);
                r = object_slot(0);
                r[0] = 10;
                r[1] = 0;
                r[2] = 10;
                r[3] = 0;
                r[4] = (uint8_t)frames[k];
                r[5] = (uint8_t)(frames[k] >> 8);
                r[6] = 0;
                r[7] = 0;
                object_render(page, 0, 0);
                for (i = 0; i < 320u * 200u; i++)
                    if (page[i]) ink++;
                check(k < 2u ? ink > 100u : ink == 0u, whats[k]);
            }
        }
        free(bank);
    }

    /* 7b3. D-61: the 0xB9 exit/igloo pass.  Level 133 has one at col 11 row 9;
     * the BD P block is stamped into the background at (176, 96), 64x64,
     * zero transparent, with level_setup_screen's add_delta_nonzero(0x1000,
     * 0x20) palette move. */
    if (level_load(132) == 0) {
        const uint8_t *bg = level_bgForProbe();
        const uint8_t *pb = level_pBlockForProbe();
        uint32_t ink = 0, bad = 0;

        check(bg && pb, "level 133 has the exit/igloo art");
        if (bg && pb) {
            for (uint32_t r = 0; r < 64u; r++)
                for (uint32_t c = 0; c < 64u; c++) {
                    uint8_t v = pb[r * 64u + c];

                    if (!v) continue;
                    ink++;
                    if (bg[(96u + r) * 512u + 176u + c] != (uint8_t)(v + 0x20u))
                        bad++;
                }
            check(ink > 500u && bad == 0u,
                  "the 0xB9 igloo is stamped at (176,96)");
        }
    }
    /* a level with no 0xB9 must not get a stamp: load its bg and check it is
     * the plain transformed BG file */
    if (level_load(42) == 0) {
        char name[32];
        uint32_t sz = 0;
        uint8_t *raw;
        uint32_t bad = 0;

        snprintf(name, sizeof name, "BACKS\\BG%u.RAW", level_bgIndexForProbe());
        raw = res_loadChunk(name, &sz, NULL);
        if (raw && sz == 512u * 320u + 768u) {
            const uint8_t *bg = level_bgForProbe();
            for (uint32_t i = 0; i < 512u * 320u; i++) {
                uint8_t v = raw[i];
                uint8_t want = (v == 0xFFu)
                                   ? (uint8_t)(0x80u + (i / 0xA00u) % 64u)
                                   : (uint8_t)(v < 0x40u ? 0x40u + v : v);

                if (!bg || bg[i] != want) bad++;
            }
            check(bad == 0u, "a level with no 0xB9 keeps its background");
        }
        free(raw);
    }

    /* 7c. the corpus: every level's header intro byte gates its post-level
     * newspaper; 14 of the 160 carry one (the corpus census) */
    {
        uint32_t loaded = 0, newspapers = 0;
        for (uint32_t id = 0; id < 160u; id++) {
            if (level_load(id) != 0) continue;
            loaded++;
            uint32_t ii = level_data()->introIndex;
            check(ii == 0xFFu || ii < 0x80u,
                  "the header intro byte is 0xFF/0..9");
            if (gameflow_postLevelScene(1, ii) != 0xFFFFu) newspapers++;
        }
        check(loaded == 160u, "all 160 level records load");
        check(newspapers == 14u, "14 levels carry a post-level newspaper");
    }

    /* 7d. check_highscore's score/record screen (START): SCORE.BIT, the
     * score line, the key, the 0x100 fade and the 0x4B hold */
    /* 8a. check_highscore's data (1000:41B8..4352): the member recount, the
     * best-time table with its 0x0999 sentinel, the NEW RECORD flag and the
     * running total the screen counts up. */
    {
        uint32_t before;
        uint32_t t1;

        hiscore_reset(); /* the shipped 0x0999 table */
        for (uint32_t i = 0; i < object_staticSlots(); i++) {
            uint8_t *r = object_slot(i);

            if (r) r[6] |= 0x80u; /* all inactive */
        }
        statusbar_setTribeDigits(1u, 2u); /* the odometer shows 12 */
        check(score_members() == 12u,
              "the member count is the odometer plus the live records");
        {
            uint8_t *r0 = object_slot(0u);

            if (r0) r0[6] &= 0x7Fu; /* one active record */
            check(score_members() == 13u, "an active record adds one");
            if (r0) r0[6] |= 0x80u;
        }
        check(score_bestFor(1u) == 0x0999u,
              "the best-time table ships as the 0x0999 sentinel");
        before = score_totalLo() | (score_totalHi() << 16);
        score_prepare();
        check(score_isRecord() == 1 &&
                  score_bestFor(1u) == statusbar_elapsedWord() &&
                  score_bestTime() == statusbar_elapsedWord(),
              "the first win beats the sentinel and stores its elapsed time");
        check(score_shownLo() == (before & 0xFFFFu) &&
                  score_shownHi() == (before >> 16),
              "the displayed total starts at the old running total");
        t1 = score_totalLo() | (score_totalHi() << 16);
        check(t1 > before, "the running total keeps the level's two terms");
        score_prepare(); /* the same elapsed time */
        check(score_isRecord() == 0,
              "an equal (not better) time is not a new record");
        check((score_totalLo() | (score_totalHi() << 16)) > t1,
              "a second level adds its terms again");
        {
            uint32_t guard = 0;

            while (score_countersRun() && guard++ < 20000u) {}
            check(guard < 20000u && score_shownLo() == score_totalLo() &&
                      score_shownHi() == score_totalHi(),
                  "1000:475D counts the display up to the running total");
        }
    }

    /* 8b. HISCORES.HUM (re_formats.md §11 / menu_loop 1000:012A): the
     * obfuscation is an involution, the shipped table is the 0x0999 bests and
     * the ten defaults, the insert shifts 18-byte entries, and the name entry
     * follows the grid's 0x1C/0x1D columns. */
    {
        uint8_t buf[600];
        uint32_t len = 0;
        char name[16];
        int32_t slot;

        for (uint32_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)i;
        hiscore_obfuscate(buf, sizeof buf);
        {
            int32_t changed = 0;
            for (uint32_t i = 0; i < sizeof buf; i++)
                if (buf[i] != (uint8_t)i) changed = 1;
            check(changed, "1000:06DE obfuscates the table");
        }
        hiscore_obfuscate(buf, sizeof buf);
        {
            int32_t same = 1;
            for (uint32_t i = 0; i < sizeof buf; i++)
                if (buf[i] != (uint8_t)i) same = 0;
            check(same, "1000:06DE is an involution (load and save share it)");
        }

        hiscore_reset();
        check(hiscore_best(1u) == 0x0999u && hiscore_best(160u) == 0x0999u,
              "every per-level best ships as 0x0999");
        check(hiscore_score(0u) == 100000u &&
                  strcmp(hiscore_name(0u), "STEVE JONES") == 0,
              "the shipped top-10 is the image's 0x4DE block");

        check(!hiscore_qualifies(9999u) && hiscore_qualifies(10000u),
              "a score equal to the last entry still displaces it");

        slot = hiscore_insert(55000u, "AB CD");
        check(slot == 5, "55000 lands between 60000 and 50000");
        check(hiscore_score(5u) == 55000u &&
                  strcmp(hiscore_name(5u), "AB CD") == 0,
              "the inserted entry carries its score and name");
        check(hiscore_score(6u) == 50000u && hiscore_score(9u) == 20000u,
              "the entries below shifted down one slot, the last dropped");

        /* the name grid: 0..0x19 = A..Z, 0x1A = space, 0x1B = '.', 0x1C =
         * delete, 0x1D = enter; 13 characters at most */
        name[0] = 0;
        check(hiscore_nameApply(name, 0u, &len) == 0 && len == 1u &&
                  name[0] == 'A',
              "column 0 types 'A'");
        hiscore_nameApply(name, 26u, &len); /* space */
        hiscore_nameApply(name, 27u, &len); /* '.'   */
        check(strcmp(name, "A .") == 0, "0x1A is space and 0x1B is '.'");
        hiscore_nameApply(name, 28u, &len);
        check(strcmp(name, "A ") == 0, "0x1C deletes the last character");
        for (uint32_t i = 0; i < 20u; i++) hiscore_nameApply(name, 1u, &len);
        check(len == 13u, "the buffer holds 13 characters");
        check(hiscore_nameApply(name, 29u, &len) == 1, "0x1D enters");

        /* the file round-trip: insert writes HISCORES.HUM encrypted, and a
         * reload reads it back through the same involution */
        hiscore_reset();
        check(hiscore_insert(77777u, "ROUND TRIP") == 3,
              "77777 lands below 80000");
        hiscore_reload();
        check(hiscore_score(3u) == 77777u &&
                  strcmp(hiscore_name(3u), "ROUND TRIP") == 0,
              "HISCORES.HUM survives a save/load round-trip");

        /* the commit's 0x1A/0x1B mapping reaches the stored name */
        hiscore_reset();
        hiscore_insert(200000u, "A\x1A"
                                "B\x1B"
                                "C");
        check(strcmp(hiscore_name(0u), "A B.C") == 0,
              "0x1A/0x1B are stored as space and '.'");
        hiscore_reset();
    }

    check(score_begin(0u) == 1, "the score screen starts");
    flow_dump_ppm("score_port.ppm", osys_null_page(0));
    /* check_highscore (1000:41ae) writes CRTC 0x12 = 0x8F: the level's
     * mode-X field goes back to the 320x200 one */
    check(osys_null_videoMode() == VIDEO_MODE_13H,
          "the score screen switches the display back to mode 13h");
    /* 1000:428D sets CRTC 0x12 = 0x8F (400 scanlines = the 320x200 field) at
     * the screen's start, so the entry mode is the card's; 0x439F's 0x8300 is
     * the full mode set at the end. */
    /* 1000:344C/0x3450: draw_level_text re-renders the world page after the
     * caption's key wait, so the caption ("YOUR TRIBE IS EXTINCT!!") is NOT
     * behind the card.  level_redrawPage must replace a sentinel page. */
    {
        uint8_t *pg = (uint8_t *)osys_null_page(0);
        uint32_t changed = 0;

        memset(pg, 0x5A, 320u * 200u);
        level_redrawPage();
        for (uint32_t i = 0; i < 320u * 200u; i++)
            if (pg[i] != 0x5A) changed++;
        check(changed > 1000u,
              "1000:344C: the end page re-renders the world over the caption");
    }
    /* 1000:4403: the card's non-zero bytes are drawn as 0xC0 + v and a zero
     * byte is LEFT ALONE - the page keeps what the level caption left there
     * (the live stamp cell shows 1..0xBF inside the card where the sprite is
     * transparent).  Draw onto a sentinel page and check the law directly. */
    {
        static uint8_t page[320 * 200];
        uint32_t kept = 0, drawn = 0, other = 0;

        memset(page, 0x5A, sizeof page);
        score_drawCard(page);
        for (uint32_t i = 0; i < sizeof page; i++) {
            if (page[i] == 0x5A) kept++;
            else if (page[i] >= 0xC0u) drawn++;
            else other++;
        }
        check(other == 0u && drawn != 0u && kept != 0u,
              "the card is drawn as 0xC0 + v and leaves zero bytes alone");
        /* chunky 1:1 copy: pixel (x,y) = SCORE.BIT[y*320+x] + 0xC0.  These
         * three bytes (0x28/0x04/0x22) are exactly where a plane-3 subsample
         * would give 0x29/0x04/0x04, so they pin the linear read. */
        check(page[18u * 320u + 18u] == 0xE8u &&
                  page[40u * 320u + 60u] == 0xC4u &&
                  page[185u * 320u + 60u] == 0xE2u,
              "1000:4403: the card is SCORE.BIT read 1 byte per pixel");
    }
    /* 1000:33E2 draw_level_text's seal: the LEVEL palette's (0x14D3) first
     * 0x80 entries become grey (77R+151G+28B)>>9 (hash_three_bytes' CH after
     * SHR CH,1); entries 0x80.. keep their colour.  check_highscore's fade
     * target is that palette plus the card's 0xC0 bank. */
    {
        static uint8_t pal[768];
        uint32_t bad = 0, kept = 0;

        for (uint32_t i = 0; i < 256u; i++) {
            pal[i * 3u + 0] = (uint8_t)(i & 0x3Fu);
            pal[i * 3u + 1] = (uint8_t)((i * 3u) & 0x3Fu);
            pal[i * 3u + 2] = (uint8_t)((i * 5u) & 0x3Fu);
        }
        palette_sealTriples(pal, 0u, 0x80u);
        for (uint32_t i = 0; i < 0x80u; i++) {
            uint32_t want = (77u * (i & 0x3Fu) + 151u * ((i * 3u) & 0x3Fu) +
                             28u * ((i * 5u) & 0x3Fu)) >>
                            9;

            if (pal[i * 3u] != want || pal[i * 3u + 1u] != want ||
                pal[i * 3u + 2u] != want)
                bad++;
        }
        check(bad == 0u, "1000:33E2: the first 0x80 triples seal to grey");
        (void)kept;
    }
    {
        InputState k;
        memset(&k, 0, sizeof k);
        k.lastKey = 1;
        /* 8a-digit. 1000:462D's digit law: block `digit*256` of SCORE.SPR, 16 rows
     * x 4 bytes per plane pass.  The pass restores DI but NOT SI, so pass p
     * reads the slot's quarter p and the glyph is a 16x16 chunky image stored
     * plane-major: pixel 4k+p = src[p*64 + r*4 + k], drawn 0xC0 | v. */
        {
            static uint8_t page[320 * 200];
            static uint8_t page2[320 * 200];

            memset(page, 0, sizeof page);
            score_column(page, 3u, 0, 0, 1u);
            check(page[0 * 320u + 0] == 0xC2u && page[0 * 320u + 15] == 0xC2u,
                  "1000:462D: digit 3's row 0 is all 0xC2");
            check(page[2 * 320u + 0] == 0xC2u && page[2 * 320u + 4] == 0xE9u &&
                      page[2 * 320u + 12] == 0xCCu &&
                      page[2 * 320u + 3] == 0xC9u,
                  "the glyph is plane-major (row 2: 02 0a 0a 09 ...)");
            /* 1234 in 5 digits is "01234", right to left in 16-px steps */
            memset(page, 0, sizeof page);
            memset(page2, 0, sizeof page2);
            score_column(page, 1234u, 0, 0, 5u);
            score_column(page2, 0u, 0, 0, 1u);
            score_column(page2, 1u, 16, 0, 1u);
            score_column(page2, 2u, 32, 0, 1u);
            score_column(page2, 3u, 48, 0, 1u);
            score_column(page2, 4u, 64, 0, 1u);
            check(memcmp(page, page2, sizeof page) == 0,
                  "a 5-digit column is zero-padded and 16 px per digit");
        }

        /* 1000:44BB draw_scorenum_three_passes: the elapsed and best times are
         * three SCORENUM digits at row 181, px 80..87 and px 240..247, every
         * byte stored as v | 0xC0 (0x450F: 8 rows x 2 page bytes, +0x58/row). */
        {
            static uint8_t a[320 * 200], b[320 * 200];
            uint32_t ina = 0, outside = 0, low = 0, diff = 0;

            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            score_drawNum3(a, 0x50u, 0x0123u, 0xB5u, 0u);
            score_drawNum3(b, 0x50u, 0x0456u, 0xB5u, 0u);
            for (uint32_t i = 0; i < sizeof a; i++) {
                uint32_t y = i / 320u, x = i % 320u;
                if (a[i]) {
                    if (y < 181u || y >= 189u || x < 104u || x >= 128u)
                        outside++;
                    else if (a[i] < 0xC0u) low++;
                    else ina++;
                }
                if (a[i] != b[i]) diff++;
            }
            check(ina != 0u && outside == 0u && low == 0u,
                  "1000:44BB draws the time digits at row 181, px 104..127");
            /* the first digit of 0x0123 is slot 1; its row 0 is
             * 17 17 02 02 02 02 17 17 -> D7 D7 C2 C2 C2 C2 D7 D7 */
            check(a[181u * 320u + 104u] == 0xD7u &&
                      a[181u * 320u + 105u] == 0xD7u &&
                      a[181u * 320u + 106u] == 0xC2u,
                  "1000:450F: the SCORENUM slot is a chunky 8x8 image");
            check(diff != 0u, "a different time draws different digits");
            memset(a, 0, sizeof a);
            score_drawNum3(a, 0xF0u, 0x0123u, 0xB5u, 0x280u);
            {
                uint32_t hit = 0;

                for (uint32_t y = 181u; y < 189u; y++)
                    for (uint32_t x = 264u; x < 288u; x++)
                        if (a[y * 320u + x]) hit++;
                check(hit != 0u,
                      "the best time sits at px 264 with BP = 0x280");
            }
        }

        /* 1000:475D's tail + [0x1DC8] = 0x15E: once both terms are spent the
         * screen runs its own countdown and then ends by itself. */
        {
            uint32_t ticks = 0;

            score_prepare();
            while (score_countersRun() && ticks < 100000u) ticks++;
            check(ticks >= 0x15Eu + 0x50u && ticks < 100000u,
                  "1000:475D: the screen ends on its own 0x15E countdown");
        }

        /* 1000:46FC: the NEW RECORD stamp - apl\RECORD.SPR's 16x24 cell drawn
         * at row 176, byte column 32 (px 128..191), one value per 4-px group,
         * non-zero as 0xC0 + v.  The shipped file's values are only {0, 2, 3},
         * so every set pixel must be 0xC2 or 0xC3, and its first four rows are
         * empty. */
        {
            static uint8_t page[320 * 200];
            uint32_t set = 0, bad = 0, top = 0, outside = 0;

            memset(page, 0, sizeof page);
            score_stampDraw(page, 1);
            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x++) {
                    uint8_t v = page[y * 320u + x];

                    if (!v) continue;
                    if (x < 128u || x >= 192u || y < 176u || y >= 200u)
                        outside++;
                    else if (y < 178u) top++;
                    else if (v != 0xC2u && v != 0xC3u) bad++;
                    else set++;
                }
            check(set != 0u && bad == 0u && top == 0u && outside == 0u,
                  "1000:46FC: the stamp is 0xC0 + RECORD.SPR, 64x24, rows 2..");
            /* apl\RECORD.SPR is chunky 64x24: pixel (x,y) = src[y*64+x].
             * Row 4's ink starts at x 32; its row profile is 0xFE7FFC. */
            {
                uint32_t inkRow[24] = { 0 };
                uint32_t profile = 0;

                for (uint32_t y = 176u; y < 200u; y++)
                    for (uint32_t x = 128u; x < 192u; x++)
                        if (page[y * 320u + x]) {
                            inkRow[y - 176u] = 1;
                            break;
                        }
                for (uint32_t r = 0; r < 24u; r++)
                    if (inkRow[r]) profile |= (1u << r);
                check(profile == 0x00FE7FFCu,
                      "the stamp is the sprite's chunky 64x24 row profile");
                check(page[180u * 320u + 128u + 32u] == 0xC2u &&
                          page[180u * 320u + 128u + 31u] == 0u,
                      "the stamp's row 4 ink starts at sprite x 32");
            }
            /* 0x4697/0x46A3: the cell is saved to A000:0000 - which is
             * OUTSIDE the visible page ([0x4106] = 0x1230 live) - the sprite
             * is drawn, and the off phase copies the scratch back.  A uniform
             * cell restores exactly. */
            memset(page, 0, sizeof page);
            for (uint32_t y = 0; y < 24u; y++)
                for (uint32_t x = 0; x < 64u; x++) page[y * 320u + x] = 0x11u;
            for (uint32_t y = 176u; y < 200u; y++)
                for (uint32_t x = 128u; x < 192u; x++)
                    page[y * 320u + x] = 0x5Au;
            score_stampDraw(page, 1);
            {
                uint32_t kept = 0, bad = 0;

                for (uint32_t y = 0; y < 24u; y++)
                    for (uint32_t x = 0; x < 64u; x++) {
                        if (page[y * 320u + x] == 0x11u) kept++;
                        else bad++;
                    }
                check(kept == 64u * 24u && bad == 0u,
                      "1000:4697: the scratch is not the visible page");
            }
            score_stampDraw(page, 0);
            {
                uint32_t kept = 0, other = 0;

                for (uint32_t y = 176u; y < 200u; y++)
                    for (uint32_t x = 128u; x < 192u; x++) {
                        if (page[y * 320u + x] == 0x5Au) kept++;
                        else other++;
                    }
                check(kept == 64u * 24u && other == 0u,
                      "the off phase restores the cell");
            }
            /* The cell is saved and restored PER PIXEL: the live capture
             * (2026-11-02) puts the card back byte-identically after the
             * blink, 1-px detail included, so a group's four pixels must
             * survive the round-trip. */
            memset(page, 0, sizeof page);
            for (uint32_t y = 176u; y < 200u; y++)
                for (uint32_t x = 128u; x < 192u; x++)
                    page[y * 320u + x] = (uint8_t)(0x10u + (x & 3u));
            score_stampDraw(page, 1);
            score_stampDraw(page, 0);
            {
                uint32_t kept = 0, bad = 0;

                for (uint32_t y = 176u; y < 200u; y++)
                    for (uint32_t x = 128u; x < 192u; x++) {
                        if (page[y * 320u + x] == (uint8_t)(0x10u + (x & 3u)))
                            kept++;
                        else bad++;
                    }
                check(kept == 64u * 24u && bad == 0u,
                      "the restore keeps the cell's 1-px detail");
            }
        }

        /* 1000:467A: with NEW RECORD set the 0x19-frame blink toggles; at the
         * 26th tick the phase flips once (the previous 8a prepared a record) */
        if (score_isRecord()) {
            uint32_t flips = 0;

            for (uint32_t i = 0; i < 26u; i++)
                if (score_blinkTick()) flips++;
            check(flips == 1u, "1000:467A: the NEW RECORD blink toggles once");
        }

        /* 1000:4333: the screen fades in before it takes input */
        {
            uint32_t t;

            for (t = 0; t < 200u && palette_fadeActive(); t++) palette_tick();
            check(!palette_fadeActive(), "the score's fade-in completes");
        }
        check(score_tick(&k) == 1, "the score screen's first tick arms");
        check(score_tick(&k) == 1, "the score screen waits on the card");
        {
            uint32_t guard = 0;

            while (score_tick(&k) && guard++ < 5000u) palette_tick();
            check(guard < 5000u, "the score screen exits after the hold");
        }
        /* D-52: 1000:4692/1000:4694 - the expiry runs the NEW phase's handler,
         * so the stamp is drawn on the on toggle and its cell restored on the
         * off one.  A fresh screen with a NEW RECORD must show both states. */
        {
            static uint8_t cellOn[24u * 64u], cellOff[24u * 64u];
            InputState tk;
            uint8_t *pg = video_page(0);
            uint32_t t, diff;

            memset(&tk, 0, sizeof tk);
            hiscore_reset(); /* the 0x0999 sentinels */
            statusbar_start(0x0600u, 12u, 4u);
            level_requestEnd(1);
            check(score_begin(0u) == 1, "the blink screen starts");
            for (t = 0; t < 400u && palette_fadeActive(); t++) {
                (void)score_tick(&tk);
                palette_tick();
            }
            (void)score_tick(&tk); /* the arm tick */
            for (t = 0; t < 400u && scoreBlinkOn() == 0; t++) {
                (void)score_tick(&tk);
                palette_tick();
            }
            for (uint32_t r = 0; r < 24u; r++)
                for (uint32_t x = 0; x < 64u; x++)
                    cellOn[r * 64u + x] = pg[(176u + r) * 320u + 128u + x];
            for (t = 0; t < 400u && scoreBlinkOn() == 1; t++) {
                (void)score_tick(&tk);
                palette_tick();
            }
            for (uint32_t r = 0; r < 24u; r++)
                for (uint32_t x = 0; x < 64u; x++)
                    cellOff[r * 64u + x] = pg[(176u + r) * 320u + 128u + x];
            diff = 0;
            for (uint32_t i = 0; i < 24u * 64u; i++)
                if (cellOn[i] != cellOff[i]) diff++;
            check(diff != 0u,
                  "1000:4692: the off phase erases the NEW RECORD stamp");
            for (t = 0; t < 6000u && score_tick(&tk); t++) palette_tick();
        }
    }

    /* 7a. 1000:3600, the goal predicate: the feet cell, and the cell one map row
     * below it (`[DI+0x20]`), both count if they are the goal tile 0xA8. */
    {
        static uint8_t pal[768];
        uint8_t *map = (uint8_t *)level_map();
        uint8_t *r = object_slot(0u);
        uint32_t cell;
        int32_t met = 0;

        r[0] = 0x40u;
        r[1] = 0x00u;
        r[2] = 0x40u;
        r[3] = 0x00u;
        r[8] = 0x02u;
        r[9] = 0x00u; /* type 2: the gate passes */
        cell = 32u * ((0x40u + 0x0Cu) / 16u) + ((0x40u + 8u) / 16u);
        if (map && cell + 32u < 640u) {
            map[cell] = 0x00u; /* the feet are not the goal */
            map[cell + 32u] = 0xA8u; /* the row below is         */
            for (uint32_t i = 0; i < 0x8Cu + 2u; i++) {
                object_tick(pal);
                if (object_lowPhaseMatch() < 0) {
                    met = 1;
                    break;
                }
            }
            check(met, "a goal tile one row below counts (1000:3624)");
            /* negative control: the same cell cleared never meets the goal */
            map[cell + 32u] = 0x00u;
            met = 0;
            for (uint32_t i = 0; i < 0x8Cu + 8u; i++) {
                object_tick(pal);
                if (object_lowPhaseMatch() < 0) {
                    met = 1;
                    break;
                }
            }
            check(!met, "with neither cell the goal is not met");
        }
    }

    /* 7a-bis. 1000:F423's "rect" is the type row DI points at (1000:E467), not a
     * 0x6F92 span.  Its y word is far above the 320-row map in every shipped
     * row, so no window ever matches: the settle gate reduces to the 0x6EBE
     * type bit.  Assert exactly that, over every type and both halves. */
    {
        uint32_t matches = 0, types = 0;

        for (uint32_t t = 0; t < 35u; t++)
            for (uint32_t h = 0; h < 2u; h++) {
                types++;
                for (int32_t x = 0; x < 512; x += 7)
                    for (int32_t y = 0; y < 320; y += 11)
                        if (object_rowWindowMatches(t, h, x, y)) matches++;
            }
        check(types == 70u && matches == 0u,
              "no shipped row's window ever matches (1000:F423 over the row)");
    }

    /* 7b. the rest of the type rows' handlers (DS:0x6918) */
    {
        uint8_t *a = object_slot(0u);
#define rd16t(r, o) ((uint32_t)((r)[o] | ((r)[(o) + 1] << 8)))
#define wr16t(r, o, v)                                                         \
    ((r)[o] = (uint8_t)(v), (r)[(o) + 1] = (uint8_t)((v) >> 8))

        /* type 18 (0x12), input 0x06 -> normalised 0x0A -> key 0x05 -> command
         * 5 -> index 37 (1000:EDB7): the home-state transition */
        wr16t(a, 8u, 0x12u);
        wr16t(a, 4u, 0x20u);
        wr16t(a, 0xAu, 0x7u);
        wr16t(a, 0xCu, 0x7u);
        wr16t(a, 0xEu, 0x7u);
        wr16t(a, 0x10u, 0x7u);
        check(object_commandRun(a, 0x06u) == OBJ_CMD_HANDLER,
              "type 18 command 5 runs the home-state handler");
        check(rd16t(a, 4u) == 0u && rd16t(a, 8u) == 0u,
              "type 0x12 becomes frame 0 / type 0 (1000:EDD9)");
        check(rd16t(a, 0xAu) == 0u && rd16t(a, 0xEu) == 0u &&
                  rd16t(a, 0x10u) == 0u,
              "the pending words are cleared");
        check(object_actionLatched(),
              "the transition consumes the ACTION press");

        /* input 0x81 -> half 1, command 0 -> index 38 (1000:EE07): the turn,
         * which needs the ACTION press to have been released (1000:EE0C) */
        object_setActionLatched(0);
        wr16t(a, 8u, 0x12u);
        wr16t(a, 0xEu, 0u);
        check(object_commandRun(a, 0x81u) != OBJ_CMD_UNPORTED,
              "type 18 half-1 command 0 runs the turn handler");
        check(rd16t(a, 0xEu) == 4u,
              "the turn advances the direction counter by 4");

        /* input 0x01 -> half 0, command 0 -> index 39 (1000:EE3B): the door */
        a[7] = 0u; /* class 0: 1000:EEB8's `DEC DX`/`MUL DL` lands at 0x5105 */
        wr16t(a, 4u, 0x8Cu);
        check(object_commandRun(a, 0x01u) != OBJ_CMD_UNPORTED,
              "the latched latch runs the enter-the-building sequence");
        check(rd16t(a, 4u) == 0u && rd16t(a, 8u) == 0u,
              "the door vector 0 sets frame 0 / type 0");
        check(object_lastSfx() == 3u, "the entry plays effect 3 (1000:EEF2)");
        check(!object_actionLatched(), "the entry releases the latch");
        check(
            object_dsTail5105(8u) == 0xFFu && object_dsTail5105(9u) == 0xFFu &&
                object_dsTail5105(10u) == 0u && object_dsTail5105(11u) == 0u &&
                object_dsTail5105(12u) == 3u && object_dsTail5105(13u) == 0u &&
                object_dsTail5105(14u) == 0xFBu &&
                object_dsTail5105(15u) == 0xFFu,
            "the class-0 door write lands at 0x510D..0x5114 (D-24)");

        /* the `0x6E76` door table's rows 1/2 (frames 0x8D/0x8E): the image's
         * y words are -7 and -9.  The class byte is 1, so the placed record is
         * slot 8 + cls = 9 and its class byte is [0x3E5E + 1]. */
        {
            uint8_t *placed = object_slot(9u);
            static const int16_t doorY[2] = { -7, -9 };

            for (uint32_t k = 0; k < 2u; k++) {
                object_setSlotClassForProbe(1u, 0xFFu);
                memset(placed, 0, 0x12u);
                wr16t(a, 8u, 0x12u);
                wr16t(a, 4u, 0x8Cu);
                wr16t(a, 0xEu, 0u);
                a[7] = 1u;
                /* the turn re-arms [0x6F1D] */
                (void)object_commandRun(a, 0x81u);
                wr16t(a, 4u, (uint16_t)(0x8Du + k));
                (void)object_commandRun(a, 0x01u);
                check((int16_t)rd16t(placed, 0xEu) == doorY[k] &&
                          rd16t(placed, 0xCu) == (uint32_t)(2u - k),
                      "the door table's row matches 0x6E76");
                check(object_slotClass(1u) == 0x3Fu && placed[8] == 0xFFu,
                      "the door masks the class byte and marks slot 8+cls");
            }
        }

        /* the type-17 9-beat (1000:ECB7): a live beat returns at 1000:ECBB's
         * JNS without the restore; the expiry runs it. */
        wr16t(a, 8u, 0x11u);
        wr16t(a, 4u, 0x30u);
        wr16t(a, 0xAu, 5u);
        wr16t(a, 0xCu, 0u);
        wr16t(a, 0xEu, 0x111u);
        wr16t(a, 0x10u, 0x222u);
        (void)object_commandRun(a, 0u);
        check(rd16t(a, 0xAu) == 4u && rd16t(a, 4u) == 0x30u &&
                  rd16t(a, 8u) == 0x11u,
              "a live 9-beat keeps the frame and type (1000:ECBB)");
        wr16t(a, 0xAu, 0u);
        (void)object_commandRun(a, 0u);
        check(rd16t(a, 4u) == 0x111u && rd16t(a, 8u) == 0x222u &&
                  rd16t(a, 0xAu) == 0u && rd16t(a, 0xCu) == 0u,
              "the expiry restores frame/type from +0xE/+0x10 (1000:ECDD)");

        /* type 18 command 6 -> index 33 (1000:ED4F): the frame nudge toward
         * 0x8E, one step per 25 frames */
        wr16t(a, 8u, 0x12u);
        wr16t(a, 4u, 0x8Cu);
        wr16t(a, 0xAu, 0u);
        wr16t(a, 0xEu, 0x19u);
        {
            static uint8_t pal[768];

            /* 1000:F9E4 counts one tick per *frame* ([0x4103]), so the frame
             * has to advance between the calls: object_tick is what counts it. */
            for (uint32_t i = 0; i < 28u; i++) {
                (void)object_commandRun(a, 0x04u); /* normalises to key 8 */
                object_tick(pal);
            }
        }
        check(rd16t(a, 4u) == 0x8Du,
              "the 25-frame cadence steps the frame once");

        /* 1000:D9E8 (type 3's tail): the two deterministic branches.
         *   - y + 0x18 >= 0x158 takes the hard landing (0xDB15);
         *   - a 0xF0 cell one row above the probe is the platform snap. */
        {
            uint8_t *m = (uint8_t *)level_map();

            wr16t(a, 8u, 3u);
            wr16t(a, 4u, 0x20u);
            wr16t(a, 0u, 100u);
            wr16t(a, 2u, 0x150u); /* probe y 0x168 >= 0x158 */
            wr16t(a, 0xAu, 0u);
            (void)object_commandRun(a, 0x01u);
            check(rd16t(a, 0u) == 93u && rd16t(a, 4u) == 0x28u &&
                      rd16t(a, 8u) == 0x16u && rd16t(a, 0xAu) == 5u &&
                      object_lastSfx() == 4u,
                  "a probe past 0x158 takes the hard landing (1000:DB15)");
            wr16t(a, 8u, 3u); /* the crash left type 0x16 */
            wr16t(a, 0u, 100u);
            wr16t(a, 2u, 100u);
            wr16t(a, 4u, 0x20u);
            wr16t(a, 0xAu, 0u);
            if (m) m[198] = 0xF0u; /* 2*0x60 + (108 >> 4) */
            (void)object_commandRun(a, 0x01u);
            /* 1000:DA3E..0xDA61 writes only y (rowY - 0x1C); the x is left
             * alone - live level 16 drowns at (344,292), stable (D-78). */
            check(rd16t(a, 0u) == 100u && rd16t(a, 2u) == 84u &&
                      rd16t(a, 4u) == 0xCDu && rd16t(a, 8u) == 0x1Cu &&
                      rd16t(a, 0xAu) == 9u && object_lastSfx() == 6u,
                  "a 0xF0 cell one row above is the platform snap (1000:DA3E)");
            if (m) m[198] = 0u;
        }

        /* the path handlers (indices 25 / 27): a type-0x0F record whose +0xE
         * links to another record (the 1000:9CF0 give/hold form), whose +8 is
         * the progress.  1000:EBA0 nudges it through 1000:FC32. */
        {
            uint8_t *node = object_slot(1u);

            wr16t(a, 8u, 0x0Fu);
            wr16t(a, 4u, 0x20u);
            wr16t(a, 0xAu, 0u);
            wr16t(a, 0xEu, 1u * 0x12u + 4u); /* the link to slot 1 */
            memset(node, 0, 0x12u);
            /* the handler and the type-15 rows' *tail* (0xDC1F,
             * `objCmdType15Tail`) both run; the checks below read the state
             * they leave. */
            (void)object_commandRun(a, 0x08u);
            check(rd16t(node, 8u) == 1u && rd16t(a, 0xAu) == 1u,
                  "the nudge adds the actor's ramped +0xA to the node's +8");
            for (uint32_t i = 0; i < 4u; i++) (void)object_commandRun(a, 0x08u);
            check(rd16t(node, 8u) == 10u,
                  "the progress ramps 1+2+3+4 and stops at 0xA (1000:FC78)");
            (void)object_commandRun(a, 0x08u);
            check(rd16t(node, 8u) == 10u, "and stays there");
            /* the park: [node+7] & 7 == 4 refuses all further nudges */
            node[7] = 0x0Cu;
            (void)object_commandRun(a, 0x08u);
            check(rd16t(node, 8u) == 10u, "a parked node is not nudged");
            /* the arrival: a progress already past -0xF takes frame 0x4A */
            node[7] = 0u;
            wr16t(node, 8u, 0xFFF1u); /* -15 */
            (void)object_commandRun(a, 0x08u);
            check(rd16t(a, 4u) == 0x4Au && node[7] == 0x0Cu,
                  "reaching +-0xF sets frame 0x4A and parks the node");
            /* 1000:EBD0 (type 15 half-1 command 1): the advance writes the
             * node's y at +0x10 and its scaled step at +0xA (the listing's
             * `ADD DI,2` at 1000:EC45), leaving the progress word at +8 */
            node[7] = 0u;
            memset(node, 0, 0x12u);
            wr16t(node, 0u, 100u);
            wr16t(node, 2u, 100u);
            wr16t(node, 8u, 5u); /* a live progress word */
            (void)object_commandRun(a, 0x89u);
            check(rd16t(node, 0x10u) == 100u,
                  "the advance stores the node's y at +0x10");
            check(rd16t(node, 8u) == 5u && rd16t(node, 0xAu) != 0u,
                  "the advance stores its step at +0xA, not in +8 (1000:EC78)");
        }

        /* the type-12 tail's three-way choice (1000:EAE6): the span
         * end `[node+4]+[node+6]` vs the record's y, then the page byte and
         * the surface line vs the probe line `y + 0x18`. */
        {
            uint8_t *node = object_slot(1u);
            uint8_t *m = (uint8_t *)level_map();
            uint32_t solid = 0x100u, open = 0x100u, old = 0u, t;

            for (t = 0; t < 256u; t++) {
                uint32_t b = level_pageByte(t, 12u); /* sub = 108 & 0xF */
                if ((b & 0x80u) && solid == 0x100u) solid = t;
                if (!(b & 0x80u) && open == 0x100u) open = t;
            }
            check(m != NULL && solid < 0x100u && open < 0x100u,
                  "the type-12 probe tiles are in the page table");
            if (m && solid < 0x100u && open < 0x100u) {
                memset(node, 0, 0x12u);
                wr16t(a, 0u, 100u);
                wr16t(a, 0xEu, 1u); /* the type-12 tail reads slot 1 directly */
                wr16t(a, 0xAu, 4u); /* the step countdown does not expire */
                old = m[102]; /* row 3 (y = 0x38), col 6 */
                m[102] = (uint8_t)solid;
                /* (a) the span end 0x10 is above the record's y 0x30: home,
                 * even though the cell under the probe is solid */
                wr16t(a, 2u, 0x30u);
                wr16t(a, 4u, 0x20u);
                wr16t(a, 8u, 0x0Cu);
                wr16t(node, 4u, 0x10u);
                wr16t(node, 6u, 0u);
                (void)object_commandRun(a, 0u);
                check(
                    rd16t(a, 2u) == 0x30u && rd16t(a, 4u) == 0x10u &&
                        rd16t(a, 8u) == 2u,
                    "type 12: a span end above the record is home (1000:EAF9)");
                /* (b) below the record, a solid page byte: the stepper */
                wr16t(a, 2u, 0x20u);
                wr16t(a, 4u, 0x20u);
                wr16t(a, 8u, 0x0Cu);
                wr16t(node, 4u, 0x30u);
                wr16t(node, 6u, 0x20u);
                (void)object_commandRun(a, 0u);
                check(
                    rd16t(a, 2u) == 0x21u && rd16t(a, 4u) == 0x20u &&
                        rd16t(a, 8u) == 0x0Cu,
                    "type 12: a solid cell takes the down-stepper (1000:EB4B)");
                /* (c) below the record, the surface line inside the 0x18 band:
                 * home against the probe line, not against the record's y */
                wr16t(a, 2u, 0x20u);
                wr16t(a, 4u, 0x20u);
                wr16t(a, 8u, 0x0Cu);
                m[102] = (uint8_t)open;
                (void)object_commandRun(a, 0u);
                check(rd16t(a, 2u) == 0x20u && rd16t(a, 4u) == 0x10u &&
                          rd16t(a, 8u) == 2u,
                      "type 12: the surface is compared with the probe line");
                m[102] = (uint8_t)old;
            }
        }

        /* 1000:DC1F, the type-15 rows' tail: a placed record inside the node's
         * window (|dy| <= 8 of node.y + 0x10, |dx| <= 1 of node.x + 8, type
         * <= 2) becomes the carried form (frame 0x55, type 0x10, +0xE =
         * 0xF0 + ([0x4103] & 0x3F)) with effect 8.  Type 15 command 0 has no
         * handler, so the run reports the tail alone. */
        {
            uint8_t *node = object_slot(1u);
            uint8_t *target = object_slot(2u);

            wr16t(a, 8u, 0x0Fu);
            wr16t(a, 4u, 0x20u);
            wr16t(a, 0xEu, 1u * 0x12u + 4u); /* the link to slot 1 */
            memset(node, 0, 0x12u);
            wr16t(node, 0u, 100u);
            wr16t(node, 2u, 100u);
            wr16t(node, 8u, 1u); /* a non-zero progress */
            memset(target, 0, 0x12u);
            wr16t(target, 0u, 108u); /* node.x + 8 */
            wr16t(target, 2u, 92u); /* node.y - 8 */
            wr16t(target, 8u, 1u); /* type 1 */
            check(
                object_commandRun(a, 0x01u) == OBJ_CMD_TAIL,
                "a type-15 command with no handler reports its tail (1000:DC1F)");
            check(rd16t(target, 4u) == 0x55u && rd16t(target, 8u) == 0x10u,
                  "the record in the window takes the carried form");
            check(rd16t(target, 0xAu) == 0u && rd16t(target, 0xCu) == 0u,
                  "the carried form clears +0xA and +0xC");
            /* a word add, so the phase (0..0x3F) can carry past 0xFF */
            check(rd16t(target, 0xEu) >= 0xF0u && rd16t(target, 0xEu) <= 0x12Fu,
                  "the carried form's +0xE is 0xF0 + the frame phase");
            check(object_lastSfx() == 8u, "the carried form plays effect 8");
            memset(target, 0, 0x12u);
            memset(node, 0, 0x12u);
        }

        /* 1000:DD3C, the give exit: when the record in the window is the
         * player's, the actor's class slot is freed and the actor is re-framed
         * at that class's placed record (frame 0x103, type 0x1E, +0xA = 6). */
        {
            uint8_t *node = object_slot(1u);
            uint8_t *target = object_slot(2u);
            uint8_t *slotRec = object_slot(9u);

            wr16t(a, 8u, 0x0Fu);
            a[7] = 1u; /* class id 1 -> record 8+1 */
            wr16t(a, 0xEu, 1u * 0x12u + 4u);
            memset(node, 0, 0x12u);
            wr16t(node, 0u, 100u);
            wr16t(node, 2u, 100u);
            wr16t(node, 8u, 1u);
            memset(target, 0, 0x12u);
            wr16t(target, 0u, 108u);
            wr16t(target, 2u, 92u);
            wr16t(target, 8u, 1u);
            memset(slotRec, 0, 0x12u);
            wr16t(slotRec, 0u, 200u);
            wr16t(slotRec, 2u, 200u);
            object_setPlayer(2u);
            (void)object_commandRun(a, 0x01u);
            check(rd16t(a, 0u) == 200u && rd16t(a, 2u) == 192u,
                  "the give exit takes the class record's position, y - 8");
            check(rd16t(a, 4u) == 0x103u && rd16t(a, 8u) == 0x1Eu,
                  "the give exit sets frame 0x103 / type 0x1E");
            check(a[7] == 0u && rd16t(slotRec, 0xEu) == 0xFFFFu,
                  "the class slot is freed and its record marked removed");
            object_setPlayer(-1);
            memset(target, 0, 0x12u);
            memset(node, 0, 0x12u);
            memset(slotRec, 0, 0x12u);
        }

        /* type 0x17 half-1 command 0 -> index 48 (1000:EFD4, the turn), then
         * half-0 command 0 -> index 49 (1000:EFD7, the idle form): with the
         * latch set that is the LEAVE sequence 1000:EFE4, which nudges the
         * record and runs the fall machine 1000:F6F6.  A 0xF0 cell at the
         * probe's row makes it the platform branch. */
        {
            uint8_t *m = (uint8_t *)level_map();

            object_setActionLatched(0);
            wr16t(a, 8u, 0x17u);
            wr16t(a, 4u, 0x20u);
            wr16t(a, 0u, 106u);
            wr16t(a, 2u, 100u);
            wr16t(a, 0xEu, 0u);
            check(object_commandRun(a, 0x81u) != OBJ_CMD_UNPORTED &&
                      rd16t(a, 0xEu) == 4u,
                  "type 0x17 half-1 command 0 arms the turn (+0xE = 4)");
            if (m)
                m[198] = 0xF0u; /* 2*(rowY-0x10) + (cx>>4), cx=108, rowY=112 */
            check(object_commandRun(a, 0x01u) != OBJ_CMD_UNPORTED,
                  "the latched idle command runs the leave sequence");
            /* frame 0x20 != 0xBB, so the leave nudges LEFT (x -= 4) and then
             * xoff = -2: AX = 100 (1000:F736).  The splash x is
             * ((AX + 8) & ~0xF) - 4 + 8 = 100 (1000:F772) - reading the probe
             * CX instead would give 116.  y = rowY - 0x1C = 84. */
            check(
                rd16t(a, 0u) == 100u && rd16t(a, 2u) == 84u,
                "the platform branch snaps to AX, not CX (1000:F772): x 100, y 84");
            check(rd16t(a, 4u) == 0xCDu && rd16t(a, 8u) == 0x1Cu,
                  "the platform branch sets frame 0xCD / type 0x1C");
            check(rd16t(a, 0xAu) == 9u && object_lastSfx() == 6u,
                  "a nine-frame hold and effect 6");
            if (m) m[198] = 0u;
        }

        /* type 0 half-1 command 2 -> index 29 (1000:EC99): the walk, then the
         * neighbour pass.  A class-4 neighbour inside the window whose type
         * magnitude is over 0xA is skipped (1000:FB7E); one under it takes the
         * caller's +0xA delta (1000:FC21) or stays, depending on the item word's
         * bit for its fine x. */
        {
            uint8_t *nb = object_slot(9u);

            wr16t(a, 8u, 0u);
            wr16t(a, 4u, 0x10u);
            wr16t(a, 0u, 0x40u);
            wr16t(a, 2u, 0x40u);
            object_setSlotClass(1u, 4u); /* live record 9's class */
            wr16t(nb, 0u, 0x46u); /* the window of the probe */
            wr16t(nb, 2u, 0x44u);
            wr16t(nb, 8u, 0x20u); /* |type| > 0xA: skipped */
            wr16t(nb, 6u, 0u);
            check(object_commandRun(a, 0x88u) != OBJ_CMD_UNPORTED &&
                      rd16t(nb, 8u) == 0x20u,
                  "a neighbour with |type| > 0xA is skipped");
            wr16t(nb, 8u, 0x8u);
            wr16t(a, 0u, 0x40u);
            (void)object_commandRun(a, 0x88u);
            check(rd16t(nb, 8u) == 0x8u || rd16t(nb, 8u) == 0x12u,
                  "a neighbour under the gate takes +0xA or keeps its type");
            object_setSlotClass(1u, 5u); /* restore the spear's class */
        }
#undef rd16t
#undef wr16t
    }

    /* 8. the camera follower (1000:5CB6): target = record - (0x88, 0x44),
     * clamped to the pan range (0xC0, 0xA8), 4 px per axis per frame; the
     * record's offset pair ([0x2D90] -> 0x2D92) is {0,0} at rest */
    {
        uint8_t *rec = object_slot(0u);

        object_setActionEntity(0u);
        level_setView(0u, 0u);
        /* 1000:5D37's walk is idle while [0x2D8E]'s bit 15 is set; the fall
         * machine may have cleared it in an earlier test, so start from rest. */
        level_set_offsetWord(0xFFFFu);
        level_reset_offsetWalk();
        rec[0] = (uint8_t)(0x88u + 0x20u);
        rec[1] = 0x00u; /* (0x20,0x10) */
        rec[2] = (uint8_t)(0x44u + 0x10u);
        rec[3] = 0x00u;
        level_followView();
        check(level_viewX() == 4u && level_viewY() == 4u,
              "the follower moves 4 px an axis (0x5D09/0x5D21)");
        for (uint32_t i = 0; i < 16u; i++) level_followView();
        check(level_viewX() == 0x20u && level_viewY() == 0x10u,
              "the follower settles on record - (0x88, 0x44)");

        rec[0] = 0x68u;
        rec[1] = 0x01u; /* 0x0168: past 0xC0+0x88 */
        rec[2] = 0x04u;
        rec[3] = 0x01u; /* 0x0104: past 0xA8+0x44 */
        for (uint32_t i = 0; i < 64u; i++) level_followView();
        check(level_viewX() == 0xC0u && level_viewY() == 0xA8u,
              "the pan clamps to 512-320 px x 320-152 rows");

        rec[0] = 0x10u;
        rec[1] = 0x00u; /* left of the anchor */
        rec[2] = 0x10u;
        rec[3] = 0x00u;
        for (uint32_t i = 0; i < 64u; i++) level_followView();
        check(level_viewX() == 0u && level_viewY() == 0u,
              "a record left of the anchor pins the pan at 0");
        object_setActionEntity(0u);
        level_setView(0u, 0u);
    }

#define wr16t2(r, o, v)                                                        \
    ((r)[o] = (uint8_t)(v), (r)[(o) + 1] = (uint8_t)((v) >> 8))
    /* 8b. 1000:5D37, the follower's offset walk.  The pair at [0x2D90] is added
     * to the record's position before the clamp, and the pointer advances one
     * pair per call while [0x2D8E]'s bit 15 is clear; on the last pair (0x2DA2)
     * the original writes 0x2D92 into the *gate* and decrements it, leaving the
     * pointer there - so the camera keeps the (1, 1) offset for good.  That is
     * the write objFallMachine makes at 0xF8CE (`[0x2D8E] = vx >> 3`). */
    {
        uint8_t *rec = object_slot(0u);

        object_setActionEntity(0u);
        wr16t2(rec, 0u, 0x88u + 0x20u);
        wr16t2(rec, 2u, 0x44u + 0x10u);
        level_setView(0u, 0u);
        level_set_offsetWord(0xFFFFu); /* bit 15: the walk idles */
        level_reset_offsetWalk();
        for (uint32_t i = 0; i < 16u; i++) level_followView();
        check(level_viewX() == 0x20u && level_viewY() == 0x10u,
              "an idle gate leaves the follower's target alone");
        /* 0xF8CE's write: a small vertical speed clears bit 15 */
        level_set_offsetWord(0x0000u);
        for (uint32_t i = 0; i < 16u; i++) level_followView();
        check(
            level_viewX() == 0x21u && level_viewY() == 0x11u,
            "the cleared gate walks the pairs and shifts the camera to (1, 1)");
        check(level_offsetWord() == 0x2D91u,
              "the last pair writes 0x2D91 into the gate (1000:5D47/5D4D)");
    }
#undef wr16t2

    /* 10. 1000:5C2F, the two TER.DAT slots.  object_array_init (0x4CAB) hides
     * them with 0xFFFF in their +0x10 flag words - the live game reads exactly
     * that (00 x16, ff ff, 00 00 in both slots) - so nothing is drawn until a
     * slot is enabled by hand. */
    {
        static uint8_t page[320 * 200];
        uint8_t *s0 = object_terSlotAt(0u);

        check(s0 && (s0[0x10] | (s0[0x11] << 8)) == 0xFFFFu,
              "1000:4CAB disables both TER slots (0x64AA/0x64BE = 0xFFFF)");
        memset(page, 0, sizeof page);
        object_renderTer(page, 0, 0);
        {
            uint32_t nz = 0;

            for (uint32_t i = 0; i < sizeof page; i++)
                if (page[i]) nz++;
            check(nz == 0u, "a disabled slot draws nothing (1000:587F)");
        }
        /* enable slot 0: index 0 -> TER source 0x40, y offset 6, 24 rows */
        s0[0x10] = 0;
        s0[0x11] = 0;
        s0[0] = 0x0C + 20;
        s0[1] = 0; /* x: the sprite starts at 20 */
        s0[2] = 30;
        s0[3] = 0; /* y: 30 + the y offset 6 -> 36 */
        s0[0x0C] = 0;
        s0[0x0D] = 0; /* the 0x2D7E index */
        memset(page, 0, sizeof page);
        object_renderTer(page, 0, 0);
        {
            uint32_t inside = 0, outside = 0;

            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x++) {
                    int32_t in = (x >= 20u && x < 52u && y >= 36u && y < 60u);
                    if (page[y * 320u + x]) {
                        if (in) inside++;
                        else outside++;
                    }
                }
            check(inside != 0u && outside == 0u,
                  "an enabled slot draws the 32x24 cell at (x-0xC, y+yoff)");
        }
        s0[0x10] = 0xFFu;
        s0[0x11] = 0xFFu; /* back off */
    }

    /* 9. menu_loop's MAP screen: the card loads, the high-score insert runs
     * first when the running total qualifies (1000:01CD), then the key exits. */
    hiscore_reset();
    check(map_begin() == 1, "the MAP screen starts");
    {
        InputState k;
        memset(&k, 0, sizeof k);
        k.lastKey = 1;
        check(map_tick(&k) == 1,
              "the MAP's first tick ignores the cutscene's held key");
        /* the score screen's `prepared` total qualifies, so the name grid is
         * up.  Type two letters, delete one, type again, then Enter commits. */
        {
            uint32_t total =
                ((uint32_t)score_totalHi() << 16) | score_totalLo();
            int32_t found = 0;

            check(hiscore_qualifies(total),
                  "1000:01CD: the running total qualifies for the table");
            /* 1000:024B: a qualifying total sees the seven-line page first;
             * the key that dismisses it is not typed into the name. */
            {
                static uint8_t page[320 * 200];
                uint32_t lit = 0;

                memset(page, 0, sizeof page);
                map_drawCongrats(page);
                for (uint32_t i = 0; i < 7u; i++) {
                    uint32_t y = 0x19u + 0x10u * i;
                    uint32_t n = 0;
                    for (uint32_t r = 4u; r < 14u; r++)
                        for (uint32_t x = 0; x < 320u; x++)
                            if (page[(y + r) * 320u + x]) n++;
                    check(n != 0u, "1000:024B: a congratulation line is drawn");
                }
                for (uint32_t i = 0; i < sizeof page; i++)
                    if (page[i]) lit++;
                check(lit != 0u, "the congratulation page draws text");
            }
            /* 1000:0251 MOV CX,2: the page is drawn twice with a flip between.
             * In the one-present-per-tick model the flip is the present at the
             * end of the tick; wipe the page and require the second pass to
             * restore it before the key wait. */
            check(map_congratsDraws() == 1u,
                  "1000:024B: the first congratulation pass is drawn");
            {
                uint8_t *pg = (uint8_t *)osys_null_page(0);
                uint32_t lit = 0;

                memset(pg, 0, 320u * 200u);
                k.lastKey = 0;
                check(map_tick(&k) == 1, "the flip tick keeps the page up");
                check(map_congratsDraws() == 2u,
                      "1000:0251: the second congratulation pass is drawn");
                for (uint32_t i = 0; i < 320u * 200u; i++)
                    if (pg[i]) lit++;
                check(lit != 0u,
                      "the second pass redraws the page after the flip");
            }
            /* 1000:028C..030A: the key starts five palette fades, the
             * HIGH.BIT re-copy and the two rectangles; the ring comes up
             * after the fifth.  Drive map_tick + palette_tick together. */
            {
                uint32_t f0 = map_fadeStarts();
                uint32_t guard = 0;

                k.lastKey = 'Q';
                map_tick(&k); /* dismiss the page */
                k.lastKey = 0;
                while (map_transitionStep() < 6u && guard++ < 2000u) {
                    map_tick(&k);
                    palette_tick();
                }
                check(guard < 2000u && map_transitionStep() == 6u,
                      "1000:028C..030A: the name transition completes");
                check(map_fadeStarts() - f0 == 5u,
                      "1000:028C..0307: five palette fades run in order");
                {
                    static const uint32_t want[5][4] = {
                        { 0x80u, 0x80u, 0x02u, 0x180u },
                        { 0x20u, 0x20u, 0x1Eu, 0x120u },
                        { 0x40u, 0x20u, 0x02u, 0x120u },
                        { 0x40u, 0x20u, 0x1Eu, 0x0C0u },
                        { 0x20u, 0x20u, 0x46u, 0x060u },
                    };

                    for (uint32_t i = 0; i < 5u; i++) {
                        uint32_t f = 0, e = 0, d = 0, t = 0;

                        check(
                            map_fadeAt(f0 + i, &f, &e, &d, &t) &&
                                f == want[i][0] && e == want[i][1] &&
                                d == want[i][2] && t == want[i][3],
                            "1000:028C..0307: the fade parameters are the listing's");
                    }
                }
            }
            k.lastKey = 'Q';
            map_tick(&k);
            k.lastKey = 'X';
            map_tick(&k);
            k.lastKey = 8u;
            map_tick(&k); /* delete */
            k.lastKey = 'Z';
            map_tick(&k);
            k.lastKey = 13u;
            map_tick(&k); /* Enter: commit */
            for (uint32_t i = 0; i < hiscore_count(); i++)
                if (hiscore_score(i) == total &&
                    strcmp(hiscore_name(i), "QZ") == 0)
                    found = 1;
            check(found, "the typed name and the total reach the table");

            /* draw_score_block (1000:0651): rank/name/score are centred at
             * 0x20/0x82/0x102, pitch 0x14 from base 0; the header is at
             * 0xA0/0x14 with blit_text_tile's 0x20 mask. */
            {
                static uint8_t page[320 * 200];
                static const uint32_t cx[3] = { 0x20u, 0x82u, 0x102u };

                memset(page, 0, sizeof page);
                map_drawList(page);
                for (uint32_t k = 0; k < 10u; k++) {
                    uint32_t y = k * 0x14u;
                    for (uint32_t c = 0; c < 3u; c++) {
                        uint32_t n = 0;

                        for (uint32_t r = 4u; r < 14u; r++)
                            for (uint32_t x = cx[c] - 16u; x <= cx[c] + 16u;
                                 x++)
                                if (page[(y + r) * 320u + x]) n++;
                        check(n != 0u,
                              "1000:0651: rank/name/score ink at the column");
                    }
                }
                {
                    uint32_t n = 0;

                    for (uint32_t r = 4u; r < 14u; r++)
                        for (uint32_t x = 0x80u; x <= 0xC0u; x++)
                            if (page[(0x14u + r) * 320u + x]) n++;
                    check(n != 0u, "1000:0518: the header is at (0xA0,0x14)");
                }
                /* 1000:0CBE: the name screen has the wheel and the typed
                 * name only - no list columns, no port-only NAME: line. */
                memset(page, 0, sizeof page);
                map_drawName(page);
                {
                    uint32_t low = 0, wide = 0, nameInk = 0;

                    for (uint32_t y = 160u; y < 200u; y++)
                        for (uint32_t x = 0; x < 320u; x++)
                            if (page[y * 320u + x]) low++;
                    for (uint32_t y = 0; y < 160u; y++)
                        for (uint32_t x = 240u; x < 320u; x++)
                            if (page[y * 320u + x]) wide++;
                    for (uint32_t y = 69u; y < 86u; y++)
                        for (uint32_t x = 0; x < 320u; x++)
                            if (page[y * 320u + x]) nameInk++;
                    check(
                        low == 0u && wide == 0u && nameInk != 0u,
                        "1000:0CBE: the name screen has no list and no NAME line");
                }
            }
        }
        /* 1000:0705 expand_nibbles_4plane: the 4bpp transpose.  ALPHA.SPR's
         * first non-zero word is 0xE007 at offset 16; the expander turns it
         * into [0, 14, 7, 0] on plane 0 and [0, 12, 15, 0] on plane 1. */
        {
            static uint8_t in[32], out[65536];

            memset(in, 0, sizeof in);
            in[16] = 0x07u;
            in[17] = 0xE0u;
            memset(out, 0, sizeof out);
            alpha_expand(in, out);
            check(out[32] == 0u && out[33] == 14u && out[34] == 7u &&
                      out[35] == 0u,
                  "1000:0705: plane 0 of 0xE007 is 0,14,7,0");
            check(out[0x4000 + 32] == 0u && out[0x4000 + 33] == 12u &&
                      out[0x4000 + 34] == 15u && out[0x4000 + 35] == 0u,
                  "1000:0705: plane 1 is 0,12,15,0");
        }

        /* the ring: rotate_spinner_cell (1000:0F93) with the image's own
         * parameters - sin table at DS:0xB8, [0xB2]/[0xB0] = 157/66 (the x),
         * [0xB6]/[0xB4] = 79/62 (the y), page stride 80. */
        {
            uint32_t src, dst;

            alpha_cell(0u, 0x10Eu, &src, &dst);
            check(dst == 997u && src == 16396u,
                  "1000:0F93: cell 0 lands at row 12, px 148, plane 1");
            alpha_cell(5u, 0x10Eu + 5u * 0x0Cu, &src, &dst);
            check(dst == 3491u && src == 33100u,
                  "cell 5 lands at row 43, px 204, plane 3");
            alpha_cell(15u, 0x10Eu + 15u * 0x0Cu, &src, &dst);
            check(dst == 10917u && src == 17356u,
                  "cell 15 (the far side of the ring) lands at row 136");
        }
        {
            static uint8_t page[320 * 200];
            uint32_t in = 0, out = 0;

            memset(page, 0, sizeof page);
            alpha_drawWheel(page, 0x10Eu);
            for (uint32_t i = 0; i < sizeof page; i++)
                if (page[i]) in++;
            check(in != 0u, "the 30-cell letter wheel draws into the page");
            flow_dump_ppm("map_ring.ppm", page);
            memset(page, 0, sizeof page);
            alpha_drawName(page, "AB");
            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x++)
                    if (page[y * 320u + x]) {
                        if (y < 70u || y >= 90u) out++;
                    }
            check(out == 0u, "the typed name is drawn at row 0x45 (72)");
            flow_dump_ppm("map_wheel.ppm", page);
            /* the glyph's bits are its pixels: a one-character name draws the
             * 'A' cell (plane 0, x & 3 == 0) - 46 set bits in the shipped
             * sheet's 16x4-byte cell - and nothing else */
            {
                uint32_t lit = 0;

                memset(page, 0, sizeof page);
                alpha_drawName(page, "A");
                for (uint32_t i = 0; i < sizeof page; i++)
                    if (page[i]) lit++;
                check(lit == 46u,
                      "the 'A' glyph is 46 pixels of the shipped alpha cell");
            }

            /* the cursor spins the ring: the selected column sits at the top */
            {
                uint32_t src, dst, top = 0xFFFFu;

                for (uint32_t i = 0; i < 30u; i++) {
                    uint32_t ang = (0x10Eu - 7u * 0x0Cu + i * 0x0Cu) % 0x168u;

                    alpha_cell(i, ang, &src, &dst);
                    if (dst / 80u < top) top = dst / 80u;
                }
                check(top <= 20u,
                      "the selected cell sits at the top of the ring");
            }
        }

        /* 1000:1251: the two rectangle outlines (top 0x40, left/bottom 0x50,
         * right 0x40) land at the listing's coordinates. */
        {
            static uint8_t page[320 * 200];

            memset(page, 0, sizeof page);
            map_drawRect(page, 0x95u, 9u, 0xFu, 0xFu);
            map_drawRect(page, 0x60u, 0x45u, 0x77u, 0xFu);
            check((page[9u * 320u + 0x95u] & 0x40u) != 0u &&
                      (page[(9u + 0xFu) * 320u + 0x95u] & 0x50u) != 0u &&
                      (page[9u * 320u + 0x95u + 0xFu] & 0x40u) != 0u &&
                      (page[20u * 320u + 0x95u] & 0x50u) != 0u,
                  "1000:1251: the first outline is at (0x95,9) 0xF x 0xF");
            check((page[0x45u * 320u + 0x60u] & 0x40u) != 0u &&
                      (page[(0x45u + 0xFu) * 320u + 0x60u] & 0x50u) != 0u &&
                      (page[0x45u * 320u + 0x60u + 0x77u] & 0x40u) != 0u &&
                      (page[0x45u * 320u + 0x60u + 0xFu] & 0x50u) != 0u,
                  "1000:1251: the name outline is at (0x60,0x45) 0x77 x 0xF");
        }
        /* 1000:0518: the entry animation runs [0x8AE] from 0x130 to 2 - about
         * 0x12E frames - and the panel loop only starts after it. */
        {
            uint32_t guard = 0;

            k.lastKey = 0;
            while (map_panel == 0u && guard++ < 1000u) map_tick(&k);
            check(guard >= 0x12Eu && map_boxBottom == 2u && map_panel == 1u,
                  "1000:0518: the frame unrolls from row 0x130 to row 2");
        }
        /* 1000:05F4/0613: a panel step is 0x14 frames of one [0x8AE] unit
         * each; [0x8A2] changes only after the 0x14th. */
        {
            uint32_t before = map_boxBottom;

            k.joyBits = 0x01u;
            map_tick(&k);
            check(map_boxBottom == before && map_panel == 1u,
                  "1000:060C: the step tick only arms the 0x14-frame walk");
            k.joyBits = 0;
            for (uint32_t i = 0; i < 0x14u; i++) {
                map_tick(&k);
                if (i + 1u < 0x14u)
                    check(map_boxBottom == before + i + 1u && map_panel == 1u,
                          "1000:0613: the frame moves one unit per tick");
            }
            check(map_boxBottom == before + 0x14u && map_panel == 2u,
                  "1000:060C: after 0x14 frames the panel is 2");
            for (uint32_t p = 2u; p < 6u; p++) {
                k.joyBits = 0x01u;
                map_tick(&k);
                k.joyBits = 0;
                for (uint32_t i = 0; i < 0x14u; i++) map_tick(&k);
            }
            check(map_panel == 6u && map_boxBottom == 2u + 5u * 0x14u,
                  "the panel stops at 6 with the frame at 2 + 5*0x14");
            /* down reverses the same 0x14-frame walk */
            k.joyBits = 0x02u;
            map_tick(&k);
            k.joyBits = 0;
            for (uint32_t i = 0; i < 0x14u; i++) map_tick(&k);
            check(
                map_panel == 5u && map_boxBottom == 2u + 4u * 0x14u,
                "1000:05ED: a down step walks the frame back over 0x14 frames");
        }
        /* 1000:07E6: the five frame rows, at rows 46/66/86/106/126 (the
         * 0xE66/0x14A6/0x1AE6/0x2126/0x2766 offsets on an 80-byte stride),
         * each a 14-row band built from the 4/+7/31/+9/15/+0xE runs (the
         * advance per row is 4+7+31+9+15+0xE = 80, one mode-X row). */
        {
            static uint8_t page[320 * 200];
            static const uint32_t want[5] = { 46u, 66u, 86u, 106u, 126u };
            uint32_t hit[5] = { 0, 0, 0, 0, 0 }, stray = 0;
            int32_t inBlock = 0;

            memset(page, 0, sizeof page);
            map_drawBoxEdges(page, 2u);
            for (uint32_t y = 0; y < 200u; y++) {
                uint32_t n = 0;

                for (uint32_t x = 0; x < 320u; x++)
                    if (page[y * 320u + x]) n++;
                inBlock = 0;
                for (uint32_t k = 0; k < 5u; k++)
                    if (y >= want[k] && y < want[k] + 14u) {
                        inBlock = 1;
                        hit[k] += n;
                    }
                if (n && !inBlock) stray++;
            }
            check(stray == 0u,
                  "the frame blocks land at rows 46/66/86/106/126 (+14) only");
            check(hit[0] && hit[1] && hit[2] && hit[3] && hit[4],
                  "all five frame blocks are drawn");
            check(hit[3] >= hit[4],
                  "the 0xF0 block (all four pixels) is the widest mask");
        }
        flow_dump_ppm("map_frame.ppm", video_page(0));
        k.lastKey = 1;
        check(map_tick(&k) == 0, "a key leaves the MAP screen");
    }


    /* 9. the last four tails and the level-end probe, field by field.
     *    These run last: they free records and re-arm the bar. */
    {
#define R16(r, o)    ((uint32_t)((r)[o] | ((r)[(o) + 1] << 8)))
#define W16(r, o, v) ((r)[o] = (uint8_t)(v), (r)[(o) + 1] = (uint8_t)((v) >> 8))
        uint8_t *spare = object_slot(5u);
        uint8_t ent[2u + 7u * 16u]; /* D-59: the +0xA entity records */

        /* the odometer is armed from the level start (12 members, 4 required),
         * so the probes below would take the goal form; empty it first to
         * exercise 0xF5BC's taken branch, and re-arm it for the goal test. */
        statusbar_setTribeDigits(0u, 0u);
        check(!statusbar_tribeActive(),
              "an empty odometer is the taken branch");

        /* --- 1000:DBCD (type 28): the 0x6868 table walk.  The stepper only
         * fires when +0xA counts below zero, so each step re-seeds it. --- */
        memset(spare, 0, 0x12u);
        W16(spare, 8u, 28u);
        W16(spare, 4u, 0x20u);
        W16(spare, 0xCu, 0u);
        W16(spare, 0xAu, 0u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 4u) == 0xCDu && R16(spare, 0xCu) == 1u &&
                  R16(spare, 0xAu) == 9u,
              "0xDBCD: the frame takes the 0x6868 byte and the cursor steps");
        W16(spare, 0xAu, 0u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 4u) == 0xCEu && R16(spare, 0xCu) == 2u,
              "0xDBCD: the next byte (0x6868[1] = 0xCE)");
        /* the climb: at frame 0xD3 with y not on a row boundary */
        W16(spare, 4u, 0xD3u);
        W16(spare, 2u, 0x45u);
        W16(spare, 0xAu, 0u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 2u) == 0x46u,
              "0xDBCD: at 0xD3 the record climbs a pixel");
        /* and the row boundary runs the level-end probe (0xFFFF scratch pairs,
         * a non-acting record: the taken branch) */
        W16(spare, 2u, 0x40u);
        W16(spare, 0xAu, 0u);
        W16(spare, 6u, 0u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 0u) == 0xFFFFu && R16(spare, 2u) == 0xFFFFu &&
                  spare[6] == 0xFFu && R16(spare, 8u) == 0x1Bu,
              "0xDBCD: a row boundary runs 0xF5BC's taken branch");

        /* --- 1000:DE7E (type 30): the 0x687C {dx, dy, flag} walk.  Every flag
         * byte in the shipped table is 0x01, so the armed-odometer probe inside
         * this tail is unreachable with the shipped data (re_map.md). --- */
        memset(spare, 0, 0x12u);
        W16(spare, 8u, 30u);
        W16(spare, 4u, 0x20u);
        W16(spare, 0xCu, 0u);
        W16(spare, 0xAu, 0u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 0xCu) == 2u && R16(spare, 0xAu) == 6u &&
                  R16(spare, 4u) == 0x21u,
              "0xDE7E: entry {1, 6, 1} steps the cursor, hold and frame");
        W16(spare, 0xCu, 0x0Cu); /* the {-1, 0x32, 1} entry */
        W16(spare, 0xAu, 0u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 0xCu) == 0x0Eu && R16(spare, 0xAu) == 0x32u &&
                  R16(spare, 4u) == 0x20u,
              "0xDE7E: entry {-1, 0x32, 1} walks the frame down");

        /* --- 1000:DED1 (type 34): +0xA is the ENTITY link (0x8000|index),
         * not an object link; [DI+4]/[DI+C]/[DI+8] are the entity fields. --- */
        memset(ent, 0, sizeof ent);
        ent[0] = 1u;
        ent[1] = 0u; /* one entity */
        W16(ent + 2u, 4u, 2u); /* the entity's handler = 2   */
        W16(ent + 2u, 0xCu, 0x77u); /* its waypoint; +0xE matches */
        entity_loadFixed(ent);
        memset(spare, 0, 0x12u);
        W16(spare, 8u, 34u);
        W16(spare, 0xAu, 0x8000u); /* the entity 0 link */
        W16(spare, 0xEu, 0x77u);
        (void)object_commandRun(spare, 0x01u);
        check(spare[6] == 0x80u,
              "0xDED1: a +0xC == +0xE match on a 2/3 entity sets bit 7");
        spare[6] = 0x80u;
        (void)object_commandRun(spare, 0x01u);
        check(spare[6] == 0x80u && R16(spare, 4u) == 0u,
              "0xDED1: with bit 7 set a 2/3 entity holds");
        /* bit 7 set and the entity outside 2..3 → the probe (taken branch) */
        W16(ent + 2u, 4u, 5u);
        entity_loadFixed(ent);
        W16(spare, 0x10u, 0x40u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 0u) == 0xFFFFu && spare[6] == 0xFFu,
              "0xDED1: bit 7 + a non-2/3 entity runs the probe");
        /* bit 7 clear and the entity outside 2..3 → the home form */
        memset(spare, 0, 0x12u);
        W16(spare, 8u, 34u);
        W16(spare, 0xAu, 0x8000u);
        W16(spare, 4u, 0x21u);
        W16(spare, 0xCu, 0x55u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 4u) == 0x10u && R16(spare, 8u) == 2u &&
                  R16(spare, 0xAu) == 0u && R16(spare, 0xCu) == 0u,
              "0xDED1: bit 7 clear + a non-2/3 entity is the home form");

        /* --- 1000:EF66 (type 22): the 5-frame step to frame 0x2E, then the
         * probe.  With the odometer empty the record is taken. --- */
        memset(spare, 0, 0x12u);
        W16(spare, 8u, 22u);
        W16(spare, 4u, 0x2Du);
        W16(spare, 0xAu, 0u);
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 4u) == 0x2Eu && R16(spare, 0xAu) == 5u &&
                  R16(spare, 0u) == 0xFFFFu && R16(spare, 8u) == 0x1Bu,
              "0xEF66: the step to 0x2E runs 0xF5BC's taken branch");

        /* --- 1000:F5BC's goal branch: with the tribe odometer showing a count
         * the record takes the goal form instead (frame 0x2D / type 0x21). --- */
        statusbar_setTribeDigits(0u, 4u); /* the digits show "04" */
        check(statusbar_tribeActive(),
              "the odometer is armed for the goal form");
        W16(ent + 2u, 4u, 5u);
        W16(ent + 2u, 8u, 50u); /* -> [0x4222] = 50 + 0x28 */
        entity_loadFixed(ent);
        memset(spare, 0, 0x12u);
        W16(spare, 8u, 34u);
        W16(spare, 0xAu, 0x8000u);
        W16(spare, 0x10u, 100u); /* -> [0x4220] */
        W16(spare, 0xEu, 0u);
        spare[6] = 0x80u; /* bit 7: the probe path */
        (void)object_commandRun(spare, 0x01u);
        check(R16(spare, 4u) == 0x2Du && R16(spare, 8u) == 0x21u &&
                  R16(spare, 0xAu) == 5u && R16(spare, 0u) == 92u,
              "0xF5BC: the armed odometer takes the goal form at [0x4220] - 8");
#undef R16
#undef W16
    }

    /* 7b12b. D-88: the behaviour-5 stepper now runs 1000:E039 (the mover's
     * move scan).  Level 1's spear, vx = 4, on the free cell (22,1): the scan
     * adds +0xC to x and 1000:CDF4 picks frame 0x5C.  (-0x10,+1) picks 0x64 -
     * the original's 8*f([+0xE]) + f([+0xC]) (D-116).  vx = -100 at x = 0 puts
     * the projection off the page and 1000:E27A hides the record and clears its
     * class byte. */
    if (level_load(0) == 0) {
        uint8_t *sp = NULL;
        uint32_t i, k;

        for (i = 8u; i <= object_liveCount(); i++) {
            if ((object_slotClass(i - 8u) & 0x3Fu) == 5u) {
                sp = object_slot(i);
                break;
            }
        }
        check(sp != NULL, "level 1's spear is behaviour 5");
        if (sp) {
            sp[0] = 0x60u;
            sp[1] = 0x01u;
            sp[2] = 16u;
            sp[3] = 0u;
            sp[4] = 0x58u;
            sp[5] = 0u;
            sp[8] = 0xFFu;
            sp[9] = 0xFFu;
            sp[10] = 0u;
            sp[11] = 0u;
            sp[12] = 4u;
            sp[13] = 0u;
            sp[14] = 0u;
            sp[15] = 0u;
            /* the stepper runs on odd [0x4103]; two rounds cover both parities
             * and leave the frame counter where the next test expects it */
            for (k = 0; k < 2u; k++) {
                object_tick(NULL);
                object_updateList();
            }
            check(
                (sp[0] | (sp[1] << 8)) == 356u &&
                    (sp[4] | (sp[5] << 8)) == 0x5Cu,
                "1000:E039 moves the spear by its +0xC and 0xCDF4 picks 0x5C");
            sp[4] = 0x58u;
            sp[5] = 0u;
            sp[12] = 0xF0u;
            sp[13] = 0xFFu;
            sp[14] = 1u;
            sp[15] = 0u;
            for (k = 0; k < 2u; k++) {
                object_tick(NULL);
                object_updateList();
            }
            check((sp[4] | (sp[5] << 8)) == 0x64u,
                  "0xCDF4 indexes 8*f([+0xE]) + f([+0xC]) (D-116)");
            sp[0] = 0u;
            sp[1] = 0u;
            sp[2] = 16u;
            sp[3] = 0u;
            sp[4] = 0x58u;
            sp[5] = 0u;
            sp[8] = 0xFFu;
            sp[9] = 0xFFu;
            sp[12] = 0x9Cu;
            sp[13] = 0xFFu;
            sp[14] = 0u;
            sp[15] = 0u;
            for (k = 0; k < 2u; k++) {
                object_tick(NULL);
                object_updateList();
            }
            check(sp[6] == 0xFFu && object_slotClass(1u) == 0u,
                  "1000:E039 kills an off-page mover (1000:E27A)");
        }
    }

    /* 7b12c. D-97/D-99: 1000:D41B's water branch and the 0x666B op dispatch.
     * Level 3's water tile 0xF0 at (14,17) makes a projectile whose projected
     * cell is (14,18) take the drown arm: the target gets frame 0xCD, +8 =
     * 0x1C, +0xA = 9, and the projectile is hidden.  Then a projectile with a
     * live target and vx = 0 dispatches 0x666B's op 1 (0x1000:D2AE), which sets
     * the hit record's frame to 0x50 through 1000:D23F. */
    if (level_load(2) == 0) {
        uint8_t *p = object_slot(9), *t = object_slot(0);

        object_setSlotClass(1u, 4u);
        p[0] = 0xE0u;
        p[1] = 0u;
        p[2] = 0x18u;
        p[3] = 0x01u;
        p[4] = 0x38u;
        p[5] = 0u;
        p[6] = 0u;
        p[8] = p[9] = p[10] = p[11] = p[16] = p[17] = 0u;
        p[12] = 5u;
        p[13] = 0u;
        p[14] = 0u;
        p[15] = 0u;
        p[16] = 0x77u; /* 0xD4D4 escapes 0xCEC3 before its writeback */
        p[17] = 0x77u;
        t[0] = 100u;
        t[1] = 0u;
        t[2] = 100u;
        t[3] = 0u;
        t[4] = 0x10u;
        t[5] = 0u;
        t[6] = 0u;
        t[7] = 0u;
        t[8] = 2u;
        t[9] = 0u;
        t[10] = 0u;
        t[11] = 0u;
        object_updateList();
        check((t[4] | (t[5] << 8)) == 0xCDu && (t[8] | (t[9] << 8)) == 0x1Cu &&
                  (t[10] | (t[11] << 8)) == 9u && p[6] == 0xFFu,
              "1000:D41B's water branch writes the drown state (D-97)");
        check(p[16] == 0x77u && p[17] == 0x77u,
              "1000:D4D4's ADD SP,2/RET leaves 1000:CEC3 (D-176)");
    }
    if (level_load(0) == 0) {
        uint8_t *p = object_slot(9), *t = object_slot(0);

        object_setSlotClass(1u, 4u);
        p[0] = 16u;
        p[1] = 0u;
        p[2] = 48u;
        p[3] = 0u;
        p[4] = 0x38u;
        p[5] = 0u;
        p[6] = 0u;
        p[7] = 0u;
        p[8] = p[9] = 0u;
        p[10] = 0x10u;
        p[11] = 0u;
        p[12] = 5u;
        p[13] = 0u;
        p[14] = 0u;
        p[15] = 0u;
        p[16] = 48u;
        p[17] = 0u;
        t[4] = 0x10u;
        t[5] = 0u;
        object_updateList();
        check((t[4] | (t[5] << 8)) == 0x50u,
              "1000:D162 dispatches the 0x666B hit op (D-99)");
    }

    /* D-104/D-144: the class block is 0x3E5E..0x3E72 (21 bytes); the terrain
     * walk clears 0x3E5F..0x3E72 only. */
    {
        static uint8_t flat[640];
        static uint8_t tbl[0x25F];

        object_init();
        object_setSlotClass(0u, 0xAAu);
        object_setSlotClass(20u, 0x5Au);
        (void)object_loadTerrainObjects(flat);
        check(object_slotClass(0u) == 0xAAu && object_slotClass(20u) == 0u,
              "1000:5175 clears 0x3E5F..0x3E72, not 0x3E5E (D-104)");
        object_setSlotClass(0u, 0u);
        object_setSlotClass(20u, 0x5Au);
        object_saveTable(tbl);
        check(tbl[0x3Eu + 20u] == 0x5Au,
              "the save carries the class byte at 0x3E72 (D-144)");
        object_setSlotClass(20u, 0u);
        object_loadTable(tbl);
        check(object_slotClass(20u) == 0x5Au,
              "the restore loads the class byte at 0x3E72");
    }

    if (bad) {
        fprintf(stderr, "flow: FAIL\n");
        return 1;
    }
    printf("flow: ok (the level gate, the post-level spine, the end page)\n");
    return 0;
}
