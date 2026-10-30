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
        object_init();
        check(object_staticSlots() == 8u && object_slots() == 16u &&
              object_liveCount() == 9u && object_current() == -1,
              "the placed array is 8 static + the live ones, 9 used, none current");
        check(object_noneType1d() == -1, "no 0x1D record -> true");
        check(object_typeEquals(3u) == 0, "no current record -> false");

        /* the player at (0x100, 0x80); a neighbour 0x20 right, 8 down, type 2 */
        object_setPlayer(0);
        r = object_slot(0);
        r[0] = 0x00; r[1] = 0x01; r[2] = 0x80; r[3] = 0x00;
        object_setCurrent(0);
        check(object_findNeighbour() == -1, "an empty array has no neighbour");

        r = object_slot(3);
        r[0] = 0x20; r[1] = 0x01; r[2] = 0x88; r[3] = 0x00;  /* +0x20, +8 */
        r[8] = 0x02;                                          /* type 2    */
        check(object_findNeighbour() == 3, "the window finds the neighbour");
        check(object_typeEquals(2u) == 0, "the predicate reads the *current*");
        object_setCurrent(3);
        check(object_typeEquals(2u) == -1, "the neighbour's type matches");
        check(object_classMatches(0x15u) == 0, "its class is 0");
        object_setCurrent(0);

        /* out of the window on either axis, inactive, or type >= 3: no claim */
        r[8] = 0x03;
        check(object_findNeighbour() == -1, "a type >= 3 record is skipped");
        r[8] = 0x00;
        r[6] = 0x80;                                          /* inactive  */
        check(object_findNeighbour() == -1, "an inactive record is skipped");
        r[6] = 0x00;
        r[0] = 0x22; r[1] = 0x01;                             /* +0x22     */
        check(object_findNeighbour() == -1, "0x22 is outside |dx| <= 0x20");
        r[0] = 0x20; r[1] = 0x01;
        r[2] = 0x90; r[3] = 0x00;                             /* +0x10     */
        check(object_findNeighbour() == -1, "0x10 is outside |dy| <= 8");
        r[2] = 0x88; r[3] = 0x00;

        check(object_noneType1d() == -1, "still no 0x1D record");
        r[8] = 0x1d;
        check(object_noneType1d() == 0, "a 0x1D record falsifies it");
        r[8] = 0x02;

        /* the claim's three writes (1000:3B1E) */
        check(object_claimNeighbour() == 3, "the claim takes the neighbour");
        check(r[4] == 0x03 && r[5] == 0x01, "the neighbour's frame becomes 0x103");
        check(r[6] == 0 && r[7] == 0, "the neighbour's flags word clears");
        check(object_slot(0)[4] == 0x20 && object_slot(0)[5] == 0,
              "the current object's frame becomes 0x20");
        /* the removal is the claim's *settle* step (`neighbour[+6] = 0xFFFF`,
         * not ported): until then the record is still in the window */
        check(object_findNeighbour() == 3,
              "the three writes leave the neighbour in the window");
        r[6] = 0x80;
        check(object_findNeighbour() == -1,
              "the settle step's 0xFFFF would remove it");
        object_init();
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

        /* the spawn: recipe 0 = one use, behaviour 5, sprite 0x60, +8 0xFFFF */
        object_init();
        check(object_spawnRecipe(0u, 0x30u, 0x40u) == 9,
              "recipe 0 appends at slot 9");
        r = object_slot(9);
        check(r[0] == 0x30 && r[1] == 0 && r[2] == 0x49 && r[3] == 0,
              "x = the neighbour's, y = its + 9");
        check(r[4] == 0x60 && r[5] == 0x00, "the sprite word is 0x60");
        check(r[8] == 0xFF && r[9] == 0xFF, "+8 = 0xFFFF");
        check(object_slotClass(10u) == 5u, "its behaviour 5 lands in slot 10");
        check(object_spawnRecipe(0u, 0, 0) == -1, "recipe 0 is one-shot");
        check(object_spawnRecipe(3u, 0, 0) == 10, "recipe 3 is a second spawn");
        r = object_slot(10);
        check(r[4] == 0x38 && r[5] == 0x00 && r[8] == 0 && r[10] == 0xFF,
              "recipe 3: sprite 0x38, +8 = 0, +0xA = 0xFFFF");
        check(object_slotClass(11u) == 4u, "its behaviour 4 lands in slot 11");
        object_init();
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
                { 48, 24 }, { 208, 24 }, { 256, 136 }, { 464, 264 }
            };
            rec = object_slot(i);
            rec[0] = (uint8_t)pos[i][0]; rec[1] = (uint8_t)(pos[i][0] >> 8);
            rec[2] = (uint8_t)pos[i][1]; rec[3] = (uint8_t)(pos[i][1] >> 8);
            rec[4] = 0x10;
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
                        uint8_t got = page[(24u + row) * 320u + 48u + col];
                        if (v) { ink++; if (got != v) wrong++; }
                        else if (got) wrong++;
                    }
                check(ink > 200u && wrong == 0u,
                      "the object pass draws the caveman with the measured law");
            }
            /* and the same law at the second human (208,24) */
            {
                unsigned wrong = 0;
                for (unsigned row = 0; row < 24u; row++)
                    for (unsigned col = 0; col < 16u; col++) {
                        uint8_t v = bank[0x4800u + (col & 3u) * 0x60u +
                                         row * 4u + (col >> 2)];
                        if (page[(24u + row) * 320u + 208u + col] != v) wrong++;
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
