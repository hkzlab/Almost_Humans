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
            snprintf(dp, sizeof dp, "%s/HUMANS.001/BACKS/DICON.APL", fix_dir);
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
        for (unsigned t = 0; t < 0xA0u; t++) level_loadingTick();
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
                level_loadingTick();
                const uint8_t *page = osys_null_page(0);
                const uint8_t *L = bank + 0x400u + 0x11u * 0x180u;
                unsigned badpx = 0;
                for (unsigned r = 0; r < 24u; r++)
                    for (unsigned c = 0; c < 16u; c++)
                        if (L[r * 16u + c] && page[(0x1Cu + r) * 320u + 0xB0u + c] != L[r * 16u + c])
                            badpx++;
                check(badpx == 0u, "the 'L' block drawn at x=0xB0 (bad chars)");
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

    if (bad) { fprintf(stderr, "flow: FAIL\n"); return 1; }
    printf("flow: ok (the level gate, the post-level spine, the end page)\n");
    return 0;
}
