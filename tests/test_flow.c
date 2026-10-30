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
#include "game.h"
#include <stdio.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern const uint8_t *osys_null_page(int which);
extern const uint8_t *osys_null_palette(void);

static int bad;

static void check(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", what); bad = 1; }
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    video_init(&osys_null);
    palette_init(&osys_null);
    level_init(&osys_null);
    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }
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
    check(!gameflow_tribeTooSmall(4u, 4u),  "need 4 == 4 starts");
    check( gameflow_tribeTooSmall(4u, 3u),  "need 4 > 3 skips");
    check(!gameflow_tribeTooSmall(0u, 0u),  "no need always starts");

    /* 2. entry LAB_00CD: [0x22]++ while the 160 levels last, then COMPLETE */
    {
        unsigned lv = 0u;
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
        check(level_loadingScreen(), "the LOADING screen starts");
        /* load_sprite_pal's fade (1000:1E33): 0x80 entries toward
         * dicon+0x18, div 0x19, before the frames */
        check(palette_fadeActive(), "the LOADING screen fades its palette in");
        for (unsigned t = 0; t < 60u && palette_fadeActive(); t++) palette_tick();
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
        for (unsigned t = 0; t < 0x9Fu; t++) level_loadingTick();
        check(level_loadingScreen(), "the letters phase follows");
        {
            uint32_t sz = 0;
            char lp[512];
            snprintf(lp, sizeof lp, "%s/HUMANS.001/APL/LOADING.APL", fix_dir);
            FILE *f = fopen(lp, "rb");
            uint8_t *bank = NULL;
            if (f) {
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                bank = malloc((size_t)n);
                if (!bank || fread(bank, 1, (size_t)n, f) != (size_t)n) { free(bank); bank = NULL; }
                else sz = (uint32_t)n;
                fclose(f);
            }
            check(bank && sz >= 0x400u + 0x19u * 0x180u, "LOADING.APL readable");
            if (bank) {
                /* the state after one tick: the "L" at (0xB0, 0x1C) — the
                 * template's y=0x1A plus one dy step */
                for (unsigned k = 0; k < 0x20u; k++) level_loadingTick();
                const uint8_t *page = osys_null_page(0);
                const uint8_t *L = bank + 0x400u + 0x11u * 0x180u;
                unsigned badpx = 0;
                for (unsigned r = 0; r < 24u; r++)
                    for (unsigned c = 0; c < 16u; c++)
                        if (L[r * 16u + c] && page[(0x24u + r) * 320u + 0x60u + c] != L[r * 16u + c])
                            badpx++;
                check(badpx == 0u, "the 'L' block drawn at x=0x60 (bad chars)");
                free(bank);
            }
        }
        for (unsigned t = 0; t < 0x60u; t++) level_loadingTick();
        check(!level_loadingScreen(), "the LOADING screen finishes");
        if (level_load(0) == 0 && !level_loadingScreen()) {
            /* a fresh load starts it again */
            check(level_loadingScreen(), "a reload restarts the screen");
            for (unsigned t = 0; t < 0x100u; t++) level_loadingTick();
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
            unsigned req = level_required(), mem = level_members();
            const uint8_t *map = level_map();
            unsigned counted = 0;
            for (unsigned i = 0; i < 640u; i++) if (map[i] == 0xAAu) counted++;
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
            textpage_draw(won,  "WELL DONE! YOU FINISHED LEVEL 001", -1, 0u, 0);
            textpage_draw(gone, "YOUR TRIBE IS EXTINCT!!", -1, 0u, 0);
            check(memcmp(base, won, sizeof base) != 0, "the end page draws");
            check(memcmp(won, gone, sizeof won) != 0, "the two end texts differ");
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
            unsigned n = level_members();
            for (unsigned i = 0; i < n; i++) level_memberLost();
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
        static uint8_t testPal[768];    /* the level palette the fades return to */
        object_init();
        check(object_staticSlots() == 8u && object_slots() == 29u &&
              object_liveCount() == 8u && object_current() == -1,
              "the placed array starts at 8 used (load_terrain_b) with none current");
        check(object_noneType1d() == -1, "no 0x1D record -> true");
        check(object_typeEquals(3u) == 0, "record 0 is not type 3");

        /* the player at (0x100, 0x80); a neighbour 0x20 right, 8 down, type 2 */
        object_setPlayer(0);
        r = object_slot(0);
        r[0] = 0x00; r[1] = 0x01; r[2] = 0x80; r[3] = 0x00;
        r[6] = 0x00;                                  /* active */
        object_setCurrent(0);
        object_setActionEntity(0u);
        check(object_findNeighbour(0) == -1, "an empty array has no neighbour");

        r = object_slot(3);
        r[0] = 0x20; r[1] = 0x01; r[2] = 0x88; r[3] = 0x00;  /* +0x20, +8 */
        r[6] = 0x00;                                          /* active    */
        r[8] = 0x02;                                          /* type 2    */
        check(object_findNeighbour(0) == 3, "the window finds the neighbour");
        check(object_typeEquals(2u) == 0, "the predicate reads the *acting* record");
        object_setActionEntity(3u);
        check(object_typeEquals(2u) == -1, "the neighbour's type matches");
        check(object_classMatches(0x15u) == 0, "its class is 0");
        object_setActionEntity(0u);

        /* out of the window on either axis, inactive, or type >= 3: no claim */
        r[8] = 0x03;
        check(object_findNeighbour(0) == -1, "a type >= 3 record is skipped");
        r[8] = 0x00;
        r[6] = 0x80;                                          /* inactive  */
        check(object_findNeighbour(0) == -1, "an inactive record is skipped");
        r[6] = 0x00;
        r[0] = 0x22; r[1] = 0x01;                             /* +0x22     */
        check(object_findNeighbour(0) == -1, "0x22 is outside |dx| <= 0x20");
        r[0] = 0x20; r[1] = 0x01;
        r[2] = 0x90; r[3] = 0x00;                             /* +0x10     */
        check(object_findNeighbour(0) == -1, "0x10 is outside |dy| <= 8");
        r[2] = 0x88; r[3] = 0x00;

        check(object_noneType1d() == -1, "still no 0x1D record");
        r[8] = 0x1d;
        check(object_noneType1d() == 0, "a 0x1D record falsifies it");
        r[8] = 0x02;

        /* the claim (1000:3A6A): the gate, the scan, the writes and then the
         * whole sequence — red/white flash, blinks, the animation, the spawn
         * and the settle — driven with object_tick + palette_tick */
        check(object_claimBegin(0u, 0) == 3, "the claim takes the neighbour");
        check(r[4] == 0x03 && r[5] == 0x01, "the neighbour's frame becomes 0x103");
        check(r[6] == 0 && r[7] == 0, "the neighbour's flags word clears");
        check(object_slot(0)[4] == 0x20 && object_slot(0)[5] == 0,
              "the old current object's frame becomes 0x20");
        check(object_current() == 3, "the claim makes the neighbour current");
        check(object_claimActive() == 1, "the sequence starts");
        {
            unsigned t = 0, spawned = 0;
            while (object_claimActive() && t++ < 20000u) {
                palette_tick();
                object_tick(testPal);
            }
            check(!object_claimActive(), "the claim sequence finishes");
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
        check(object_testDispatch(0x0502u) == 0, "an empty array does not meet it");
        object_setCurrent(0);
        object_setSlotClass(1u, 5u);
        object_slot(0)[7] = 1u;                 /* the record's source slot */
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
        r[0] = 0x20; r[1] = 0x00;                    /* x = 0x20          */
        r[2] = 0x10; r[3] = 0x00;                    /* y = 0x10          */
        r[8] = 0x02;                                 /* type 2            */
        {
            /* cell = 32*((0x10+0xC)/16) + (0x20+8)/16 = 32*1 + 2 = 34 */
            unsigned cell = level_map()[34];
            check(object_mapCellEquals(cell) == -1,
                  "the map cell under the record matches");
            check(object_mapCellEquals(cell + 1u) == 0, "a different cell does not");
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
        actor[0] = (uint8_t)(448u - 8u); actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u; actor[3] = 0;
        actor[4] = 0x10; actor[6] = 0; actor[8] = 2; actor[9] = 0;
        check(object_testDispatch(0x0502u) == 0,
              "the goal is not met before the pickup");
        object_actionRun();
        check((actor[7]) == 1u, "the pickup writes the ordinal as the class");
        check(object_slotClass(1u) == 0x85u,
              "the spear's slot is marked taken (0x85)");
        check(object_testDispatch(0x0502u) == 1,
              "level 1's goal (class match, arg 5) is met after the pickup");
        /* op 4 (0xEFA1): the record's state byte must be 1 (behaviour 5, the
         * spear, whose slot 5 maps to state 1 in 0x6EB6); program 2 with the
         * cursor at 5 runs op 4, which sets the fixed frame/type */
        object_actionSetCursor(6u);          /* program 1: prog[7] = op 4  */
        actor[8] = 0x00; actor[9] = 0x00;    /* type <= 2 gates the op      */
        object_actionRun();
        check((actor[4] | (actor[5] << 8)) == 0x00BBu &&
              (actor[8] | (actor[9] << 8)) == 0x0017u,
              "op 4 sets the fixed state (frame 0xBB, type 0x17)");
        object_actionSetCursor(1u);

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
            object_actionSetCursor(2u);       /* prog[3] = 0x07 */
            object_actionRun();
            check(object_actionFault() == 7u, "op 7 is recorded when dispatched");
            check(memcmp(before, actor, sizeof before) == 0,
                  "op 7 touches no state (its targets are wild writes)");
            object_actionSetProgram(1u);
            object_actionSetCursor(1u);
            object_attachOsys(&osys_null);    /* the game's halt route back   */
        }


        /* and the level's own flow ends it as won */
        object_pendingRun();
        level_begin();
        for (unsigned t = 0; t < 4000u && level_starting(); t++) {
            level_tick();
            palette_tick();
        }
        level_tick();
        check(level_ended() && level_outcome(),
              "the pickup ends level 1 as won");

        /* op 9 (0x39E4 -> 0xEF0C) + object_find_overlap (0xF2E0): a behaviour-6
         * actor walks the live record with slot 3 in its upper-left window,
         * marks it type 1, then converts the static record overlapping it */
        if (level_load(0) == 0) {
            uint8_t *mv, *live, *victim;
            object_actionSetProgram(5u);        /* program 5: prog[2] = op 9 */
            object_actionSetCursor(1u);
            object_setSlotClass(2u, 6u);        /* class 2 -> behaviour 6    */
            object_setSlotClass(1u, 3u);        /* live ordinal 1 -> behav 3 */
            mv = object_slot(0);
            mv[0] = (uint8_t)440u; mv[1] = (uint8_t)(440u >> 8);
            mv[2] = 32u; mv[3] = 0;
            mv[4] = 0x10; mv[6] = 0; mv[7] = 2u; mv[8] = 0; mv[9] = 0;
            victim = object_slot(1);            /* a neighbour at the same x */
            victim[0] = (uint8_t)440u; victim[1] = (uint8_t)(440u >> 8);
            victim[2] = 32u; victim[3] = 0;
            victim[4] = 0x10; victim[6] = 0; victim[7] = 0;
            victim[8] = 2u; victim[9] = 0;
            live = object_slot(9);              /* the spear, +8 px right  */
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
         * hold ids 0x1E/0x1F (free) while (24,1) is a wall (0x00), so a carrier
         * on the first two enters the open state and one on the third does not */
        check(level_pageFree(0x1Eu) == 1 && level_pageFree(0x1Fu) == 1 &&
              level_pageFree(0x00u) == 0 && level_pageFree(0xB8u) == 0,
              "the page gate: free tiles vs walls (and 0xB8, an item tile)");
        {
            uint8_t *mv;
            object_init();
            object_actionSetProgram(1u);
            object_actionSetCursor(5u);        /* prog[6] = 0x03 (op 3)     */
            mv = object_slot(0);
            mv[0] = (uint8_t)(22u * 16u); mv[1] = (uint8_t)((22u * 16u) >> 8);
            mv[2] = (uint8_t)(1u * 16u); mv[3] = (uint8_t)((1u * 16u) >> 8);
            mv[4] = 0x10; mv[6] = 0; mv[7] = 1u;   /* a class -> the op runs */
            mv[8] = 0; mv[9] = 0;                  /* type 0 -> step +1     */
            object_actionRun();
            check((mv[4] | (mv[5] << 8)) == 0x008Cu &&
                  (mv[8] | (mv[9] << 8)) == 0x0012u,
                  "op 3 moves a carrier on open ground (0x8C/0x12)");
            /* one cell right: (23,1) is free but its neighbour (24,1) is not */
            object_init();
            object_actionSetProgram(1u);
            object_actionSetCursor(5u);
            mv = object_slot(0);
            mv[0] = (uint8_t)(23u * 16u); mv[1] = (uint8_t)((23u * 16u) >> 8);
            mv[2] = (uint8_t)(1u * 16u); mv[3] = (uint8_t)((1u * 16u) >> 8);
            mv[4] = 0x10; mv[6] = 0; mv[7] = 1u;
            mv[8] = 0; mv[9] = 0;
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
        for (unsigned i = 0; i < 4u; i++) {
            static const uint16_t pos[4][2] = {
                { 48, 25 }, { 208, 25 }, { 256, 137 }, { 464, 265 }
            };
            rec = object_slot(i);
            rec[0] = (uint8_t)pos[i][0]; rec[1] = (uint8_t)(pos[i][0] >> 8);
            rec[2] = (uint8_t)pos[i][1]; rec[3] = (uint8_t)(pos[i][1] >> 8);
            rec[4] = 0x10;
            rec[6] = 0x00;                        /* active */
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
                unsigned ink = 0, wrong = 0;
                for (unsigned row = 0; row < 24u; row++)
                    for (unsigned col = 0; col < 16u; col++) {
                        uint8_t v = bank[0x4800u + (col & 3u) * 0x60u +
                                         row * 4u + (col >> 2)];
                        uint8_t got = page[(25u + row) * 320u + 48u + col];
                        if (v) { ink++; if (got != v) wrong++; }
                        else if (got) wrong++;
                    }
                check(ink > 200u && wrong == 0u,
                      "the object pass draws the caveman with the measured law");
            }
            /* the non-24-row classes: the spear (frame 0x60, h 16) uses
             * block = h*4 = 0x40, at SPRITES.DAT + 0x10A0 */
            {
                unsigned wrong = 0;
                memset(page, 0, sizeof page);
                object_init();
                rec = object_slot(0);
                rec[0] = 10; rec[1] = 0; rec[2] = 10; rec[3] = 0;
                rec[4] = 0x60; rec[6] = 0; rec[8] = 0x02;
                object_render(page, 0, 0);
                for (unsigned row = 0; row < 16u; row++)
                    for (unsigned col = 0; col < 16u; col++) {
                        uint8_t v = bank[0x10A0u + (col & 3u) * 0x40u +
                                         row * 4u + (col >> 2)];
                        if (page[(10u + row) * 320u + 10u + col] != v) wrong++;
                    }
                check(wrong == 0u, "the spear frame (h=16, block h*4) is drawn");
                /* the generated frame table now carries every slot, so the
                 * carried frames draw too -- and the descriptor's ybias is added
                 * to y (select_object_tile 1000:6642: ADD DX,AX) */
                {
                    unsigned bad2 = 0;
                    memset(page, 0, sizeof page);
                    object_init();
                    rec = object_slot(0);
                    rec[0] = 20; rec[1] = 0; rec[2] = 20; rec[3] = 0;
                    rec[4] = 0x5D; rec[6] = 0; rec[8] = 0x02;
                    object_render(page, 0, 0);
                    for (unsigned row = 0; row < 11u; row++)
                        for (unsigned col = 0; col < 16u; col++) {
                            uint8_t v = bank[0x0DF0u + (col & 3u) * 44u +
                                             row * 4u + (col >> 2)];
                            if (page[(22u + row) * 320u + 20u + col] != v) bad2++;
                        }
                    check(bad2 == 0u && page[21u * 320u + 20u] == 0u,
                          "frame 0x5D (carried) draws at y + ybias");
                }
                /* mode 2 (1000:6C74): 32 px wide, block rows*8, row stride 8 --
                 * frame 0x2B is rows 20 at SPRITES.DAT + 0xD930 */
                {
                    unsigned bad3 = 0;
                    memset(page, 0, sizeof page);
                    object_init();
                    rec = object_slot(0);
                    rec[0] = 40; rec[1] = 0; rec[2] = 40; rec[3] = 0;
                    rec[4] = 0x2B; rec[6] = 0; rec[8] = 0x02;
                    object_render(page, 0, 0);
                    for (unsigned row = 0; row < 20u; row++)
                        for (unsigned col = 0; col < 32u; col++) {
                            uint8_t v = bank[0xD930u + (col & 3u) * 160u +
                                             row * 8u + (col >> 2)];
                            /* y + ybias: frame 0x2B carries ybias 4 */
                            if (page[(44u + row) * 320u + 40u + col] != v) bad3++;
                        }
                    check(bad3 == 0u, "frame 0x2B (mode 2) draws 32 px wide");
                }
                /* the four placed humans again for the second-record check */
                object_init();
                for (unsigned i = 0; i < 4u; i++) {
                    static const uint16_t pos[4][2] = {
                        { 48, 25 }, { 208, 25 }, { 256, 137 }, { 465, 265 }
                    };
                    rec = object_slot(i);
                    rec[0] = (uint8_t)pos[i][0]; rec[1] = (uint8_t)(pos[i][0] >> 8);
                    rec[2] = (uint8_t)pos[i][1]; rec[3] = (uint8_t)(pos[i][1] >> 8);
                    rec[4] = 0x10;
                    rec[6] = 0x00;
                    rec[8] = 0x02;
                }
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
            }
            /* and the same law at the second human (208,25) */
            {
                unsigned wrong = 0;
                for (unsigned row = 0; row < 24u; row++)
                    for (unsigned col = 0; col < 16u; col++) {
                        uint8_t v = bank[0x4800u + (col & 3u) * 0x60u +
                                         row * 4u + (col >> 2)];
                        if (page[(25u + row) * 320u + 208u + col] != v) wrong++;
                    }
                check(wrong == 0u, "the second placed record is drawn too");
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
        check(level_diskStatusBegin(3u) == 1, "a missing disc draws the prompt");
        check(palette_fadeActive(), "the prompt fades its palette in");
        for (unsigned t = 0; t < 80u && palette_fadeActive(); t++) palette_tick();
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

    /* 7c. the corpus: every level's header intro byte gates its post-level
     * newspaper; 14 of the 160 carry one (the corpus census) */
    {
        unsigned loaded = 0, newspapers = 0;
        for (unsigned id = 0; id < 160u; id++) {
            if (level_load(id) != 0) continue;
            loaded++;
            unsigned ii = level_data()->introIndex;
            check(ii == 0xFFu || ii < 0x80u, "the header intro byte is 0xFF/0..9");
            if (gameflow_postLevelScene(1, ii) != 0xFFFFu) newspapers++;
        }
        check(loaded == 160u, "all 160 level records load");
        check(newspapers == 14u, "14 levels carry a post-level newspaper");
    }

    /* 7d. check_highscore's score/record screen (START): SCORE.BIT, the
     * score line, the key, the 0x100 fade and the 0x4B hold */
    check(score_begin(0u) == 1, "the score screen starts");
    {
        InputState k;
        memset(&k, 0, sizeof k);
        k.lastKey = 1;
        check(score_tick(&k) == 1, "the score screen's first tick arms");
        check(score_tick(&k) == 1, "the score screen waits on the card");
        for (unsigned t = 0; t < 80u && palette_fadeActive(); t++) palette_tick();
        check(!palette_fadeActive(), "the score's fade completes");
        for (unsigned t = 0; t < 0x50u; t++) {
            if (!score_tick(&k)) break;
        }
        check(!score_tick(&k), "the score screen exits after the hold");
    }

    /* 8. menu_loop's MAP screen (START): the card loads and the key exits */
    check(map_begin() == 1, "the MAP screen starts");
    {
        InputState k;
        memset(&k, 0, sizeof k);
        k.lastKey = 1;
        check(map_tick(&k) == 1,
              "the MAP's first tick ignores the cutscene's held key");
        check(map_tick(&k) == 0, "a key leaves the MAP screen");
    }

    if (bad) { fprintf(stderr, "flow: FAIL\n"); return 1; }
    printf("flow: ok (the level gate, the post-level spine, the end page)\n");
    return 0;
}
