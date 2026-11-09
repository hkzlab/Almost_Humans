/* test_flow.c - the flow's connections (entry's state machine + level_run's
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
#include "font.h"
#include "gameflow.h"
#include "hiscore.h"
#include "level.h"
#include "map.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "save.h"
#include "score.h"
#include "statusbar.h"
#include "textpage.h"
#include "video.h"
#include "compat.h"
#include "byteorder.h"
#include "fs/fs.h"

#include <stdio.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);
extern VideoMode osys_null_video_mode(void);

extern const OSystem g_osys_null;

static int32_t _bad;

/**
 * @brief Write a page and its palette as a binary PPM for inspection.
 *
 * @param path the path
 * @param page the page buffer
 */
static void _flow_dump_ppm(const char *path, const uint8_t *page)
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

/**
 * @brief Whether a page pixel lies in one of the claim's two record boxes
 * (player 0x100,0x80 and victim 0x120,0x88 in the D-240/D-242 fixture).
 *
 * @param x the page x
 * @param y the page y
 * @param vx the view x
 * @param vy the view y
 * @return 1 inside a box
 */
static int32_t _flow_claim_rec_box(uint32_t x, uint32_t y, uint32_t vx,
                                   uint32_t vy)
{
    int32_t wx = (int32_t)(x + vx);
    int32_t wy = (int32_t)(y + vy);

    if (wx >= 0xF8 && wx < 0x118 && wy >= 0x78 && wy < 0xA8) return 1;
    if (wx >= 0x118 && wx < 0x138 && wy >= 0x80 && wy < 0xB0) return 1;
    return 0;
}

/**
 * @brief Assert one condition and count a failure.
 *
 * @param ok the condition
 * @param what the check description
 */
static void _check(int32_t ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        _bad = 1;
    }
}

/**
 * @brief the record-word helpers, also used by the walk/marker blocks below
 *
 * @param r the record buffer
 * @param o the offset
 * @return the stored word
 */
static uint32_t _tf_rd16(const uint8_t *r, uint32_t o)
{ return (uint32_t)(rd_le_u16(r + o)); }
/**
 * @brief Store one little-endian 16-bit word.
 *
 * @param r the record buffer
 * @param o the offset
 * @param v the word
 */
static void _tf_wr16(uint8_t *r, uint32_t o, uint32_t v)
{ wr_le_u16(r + o, (uint16_t)v); }

/** @brief D-86: one full-charge leap from (x, y) on the loaded level - 64 held frames
 * of the fire column (1000:EE07) then the release (1000:EFD7 -> 0xEFE4) - and
 * the leftmost x / smallest y it reaches before the leap state (type 0x1A)
 * ends.  The caller must set the level up (object_init + the action row).
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param minx the minimum x
 * @param apex the apex value
 * @param endType the end type
 * @param endFrame the end frame
 */
static void _tf_leap(int32_t x, int32_t y, uint32_t *minx, uint32_t *apex,
                     uint32_t *endType, uint32_t *endFrame)
{
    uint8_t *a;

    object_init();
    object_set_action_entity(0u);
    object_action_set_program(2u);
    object_action_set_cursor(2u); /* program 2, cell 2 = icon 4 */
    object_set_action_latched(0);
    object_set_slot_class(1u, 5u);
    a = object_slot(0);
    _tf_wr16(a, 0u, (uint32_t)x);
    _tf_wr16(a, 2u, (uint32_t)y);
    _tf_wr16(a, 4u, 0xBEu);
    _tf_wr16(a, 8u, 0x18u);
    _tf_wr16(a, 0xAu, 0u);
    _tf_wr16(a, 0xCu, 0u);
    _tf_wr16(a, 0xEu, 0u);
    _tf_wr16(a, 0x10u, 0u);
    a[7] = 9u; /* the spear */
    for (uint32_t t = 0; t < 64u; t++) {
        (void)object_command_run(a, 0x80u);
        object_tick(level_palette());
    }
    *minx = 0xFFFFu;
    *apex = 0xFFFFu;
    for (uint32_t t = 0; t < 40u; t++) {
        (void)object_command_run(a, 0x00u);
        object_tick(level_palette());
        if (_tf_rd16(a, 0u) < *minx) *minx = _tf_rd16(a, 0u);
        if (_tf_rd16(a, 2u) < *apex) *apex = _tf_rd16(a, 2u);
        if (_tf_rd16(a, 8u) != 0x1Au) break;
    }
    *endType = _tf_rd16(a, 8u);
    *endFrame = _tf_rd16(a, 4u);
}

static uint32_t _tf_warns;

/** @brief Count the font's arena warnings (the D-258 check). */
static void _tf_warn(const char *msg)
{
    (void)msg;
    _tf_warns++;
}

/**
 * @brief Run the foreground flow tests.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    res_init(&g_osys_null);
    save_init(&g_osys_null);
    hiscore_bind(&g_osys_null);
    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    level_init(&g_osys_null);
    statusbar_init(&g_osys_null); /* the charge bar draws into the band */

    /* 0. font_draw_text_bounded: a fitting line draws the same bytes as
     * draw_text; a glyph whose 16-row tile would pass the arena's end is
     * dropped, and the first drop warns once (D-258). */
    {
        static uint8_t bank[44 * 32];
        static uint8_t wide[1600], same[1600], tiny[640];
        OSystem os = g_osys_null;

        memset(bank, 0xFF, sizeof bank); /* worst-case ink */
        font_publish(bank);

        /* blit_text_tile (1000:47D5) copies all 16 glyph rows (MOV CX,0x10
         * at 1000:482e): with an all-ink glyph the cell's first and last
         * rows must be written, not only the shipped font's rows 3..14. */
        {
            static uint8_t page[VIDEO_PAGE_BYTES];
            uint32_t all = 1;

            memset(page, 0, sizeof page);
            textpage_draw_line(page, "M", 40u, 0x10u);
            for (uint32_t r = 0; r < 16u; r++) {
                uint32_t n = 0;

                for (uint32_t x = 0; x < 320u; x++)
                    if (page[(40u + r) * 320u + x]) n++;
                if (!n) all = 0;
            }
            _check(all, "1000:47D5: the line blit covers all 16 glyph rows");

            /* write mode 2's 0 bits select the latch: a blank glyph writes
             * nothing at all. */
            {
                static uint8_t blank[44 * 32];
                uint32_t kept = 1;

                memset(blank, 0, sizeof blank);
                font_publish(blank);
                memset(page, 0xCC, sizeof page);
                textpage_draw_line(page, "M", 40u, 0x10u);
                for (uint32_t i = 0; i < sizeof page; i++)
                    if (page[i] != 0xCCu) kept = 0;
                _check(kept, "the blank glyph rows leave the page untouched");
                font_publish(bank);
            }
        }

        memset(wide, 0, sizeof wide);
        memset(same, 0, sizeof same);
        font_draw_text(wide, "HELLO", 0xA0u, 0u);
        font_draw_text_bounded(same, sizeof same, "HELLO", 0xA0u, 0u);
        _check(memcmp(wide, same, sizeof wide) == 0,
               "bounded draw == unbounded when the text fits");

        os.warn = _tf_warn;
        _tf_warns = 0;
        font_init(&os);
        memset(tiny, 0xCC, sizeof tiny);
        font_draw_text_bounded(tiny, sizeof tiny, "HELLO", 0xA0u, 0u);
        _check(tiny[0] == 0xCC && tiny[sizeof tiny - 1u] == 0xCC &&
                   _tf_warns == 1u,
               "an unfitting glyph is dropped and warned about");
        font_draw_text_bounded(tiny, sizeof tiny, "HELLO", 0xA0u, 0u);
        _check(_tf_warns == 1u, "the drop warning is reported once");
        font_init(&g_osys_null);

        /* textpage_draw (1000:A08E): the 9-slice box sits at
         * left = 160 - (tiles+2)*8, top = 76 - (lines+2)*8 and the text
         * row at top+15.  Pixel-exact check of the level-text page. */
        {
            static uint8_t box[9u * 256u];
            static uint8_t page[VIDEO_PAGE_BYTES];
            uint32_t adv = font_text_width("M");
            uint32_t tiles = adv / 16u + 2u;
            uint32_t cols = tiles + 2u;
            uint32_t left = 160u - cols * 8u;
            uint32_t top = 76u - 3u * 8u; /* rows = 1 line + 2 */
            uint32_t ink = 0;

            memset(box, 0xFF, sizeof box);
            textpage_publish_box(box);
            memset(page, 0, sizeof page);
            textpage_draw(page, "M", -1, 0u, 0);
            _check(page[top * VIDEO_PAGE_W + left] != 0u,
                   "the level text box's corner is at (left, top)");
            _check(left > 0u && page[top * VIDEO_PAGE_W + left - 1u] == 0u,
                   "the box does not bleed one pixel left of its corner");
            for (uint32_t x = 128u; x < 192u; x++)
                if (page[(top + 15u) * VIDEO_PAGE_W + x]) ink++;
            _check(ink != 0u,
                   "the level text line draws at the box's top+15 row");
            textpage_publish_box(NULL);
        }

        /* object_blit_sprite (the object bank's blit): the source index is
         * (col&3)*rows*(cols/4) + row*(cols/4) + col/4 - the block stride is a
         * product that exceeds a byte for a 32x32 tile (256), so a byte-sized
         * stride transposes/garbles large sprites (the PARKVIEW dinosaur).
         * Drive it with a 32x32, all-ink, position-tagged source. */
        {
            static uint8_t spr[4u * 32u * 8u];
            static uint8_t page[VIDEO_PAGE_BYTES];
            uint32_t bad = 0;

            /* i/4 repeats every 1024 bytes but differs at +256/+512, so a
             * wrong block stride reads a different value. */
            for (uint32_t i = 0; i < sizeof spr; i++) spr[i] = (uint8_t)(i / 4u + 1u);
            memset(page, 0, sizeof page);
            object_blit_sprite(page, spr, 32u, 32u, 10, 10);
            for (uint32_t y = 10u; y < 42u; y++)
                for (uint32_t x = 10u; x < 42u; x++) {
                    uint32_t want =
                        spr[(x - 10u & 3u) * 256u + (y - 10u) * 8u +
                            ((x - 10u) >> 2)];
                    if (page[y * VIDEO_PAGE_W + x] != want) bad++;
                }
            _check(bad == 0u, "object_blit_sprite's block stride is a word");
        }
    }
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
    res_set_disc(1u);

    /* 1. level_run's gate: need > members skips the level */
    _check(!gameflow_tribe_too_small(4u, 12u), "need 4 <= 12 starts");
    _check(!gameflow_tribe_too_small(4u, 4u), "need 4 == 4 starts");
    _check(gameflow_tribe_too_small(4u, 3u), "need 4 > 3 skips");
    _check(!gameflow_tribe_too_small(0u, 0u), "no need always starts");

    /* 2. entry LAB_00CD: [0x22]++ while the 160 levels last, then COMPLETE */
    {
        uint16_t lv = 0u;
        _check(gameflow_after_level(&lv) == FLOW_LEVEL && lv == 1u,
               "level 0 -> the next level");
        lv = 158u;
        _check(gameflow_after_level(&lv) == FLOW_LEVEL && lv == 159u,
               "level 158 -> 159");
        lv = 159u;
        _check(gameflow_after_level(&lv) == FLOW_ATTRACT && lv == 159u,
               "the last level -> COMPLETE (the attract)");
    }

    /* 3. load_sprite_pal's LOADING screen (1000:1DC0): ten walkers for
     * 0xA0 frames, then the letters (blocks 0x11..0x18) - the letters land
     * as the 16x24 "L" block at x=0xB0 falling from y=0x1A. */
    if (level_load(0) == 0) {
        /* level_run 1000:3054 runs level_video_init's two fades (1000:3083)
         * before the loading screen (1000:3086), so drive the chain to it. */
        for (uint32_t t = 0; t < 400u && !level_loading_screen(); t++) {
            level_begin();
            level_tick();
            palette_tick();
        }
        _check(level_loading_screen(), "the LOADING screen starts");
        /* load_sprite_pal's fade (1000:1E33): 0x80 entries toward
         * dicon+0x18, div 0x19, before the frames */
        _check(palette_fade_active(),
               "the LOADING screen fades its palette in");
        for (uint32_t t = 0; t < 60u && palette_fade_active(); t++)
            palette_tick();
        _check(!palette_fade_active(), "the loading fade completes");
        {
            char dp[512];
            snprintf(dp, sizeof dp, "%s/HUMANS.001/BACKS/CICON.APL", fix_dir);
            FILE *df = fopen(dp, "rb");
            if (df) {
                uint8_t buf[0x198];
                size_t n = fread(buf, 1, sizeof buf, df);
                fclose(df);
                if (n == sizeof buf && buf[0] == 'A')
                    _check(memcmp(osys_null_palette(), buf + 0x18, 0x180) == 0,
                           "the loading DAC is dicon+0x18");
            }
        }
        /* the background: a clean black page plus BACKS\DICON.APL's 80x56
         * card at (0x50,0x60) above the status bar - the 1000:3310 blit's
         * visible position, 4 px/byte (not doubled) */
        level_loading_tick(); /* the first frame composes it */
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
                    _check(page[0] == 0u, "the loading background is cleared");
                    _check(page[(0x60u + 10u) * 320u + 0x78u + 40u] ==
                                   dic[0x400u + 10u * 80u + 40u] &&
                               page[(0x60u + 10u) * 320u + 0x78u + 41u] ==
                                   dic[0x400u + 10u * 80u + 41u],
                           "the style card is at (0x78,0x60), 1x");
                }
            }
        }
        for (uint32_t t = 0; t < 0x9Eu; t++) level_loading_tick();
        /* D-111 (closed 2026-11-05): 1000:1EBD..1EC7 raises every walker's
         * frame below 0x10 to 0x10 on the batch's last pass, before the draw.
         * This tick is that pass (0xA0th), so walker 1 (x 0x40 + 0xA0, y
         * 0x6E) is drawn at screen (0x90, 0x3C) with frame 0x10. */
        {
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
                }
                fclose(f);
            }
            level_loading_tick(); /* the walkers' last pass */
            if (bank) {
                const uint8_t *page = osys_null_page(0);
                const uint8_t *spr = bank + 0x400u + 0x10u * 0x180u;
                uint32_t badpx = 0, nz = 0;

                for (uint32_t r = 0; r < 24u; r++)
                    for (uint32_t c = 0; c < 16u; c++)
                        if (spr[r * 16u + c]) {
                            nz++;
                            if (page[(0x3Cu + r) * 320u + 0x90u + c] !=
                                spr[r * 16u + c])
                                badpx++;
                        }
                _check(nz != 0u && badpx == 0u,
                       "D-111: the walkers show frame 0x10 on the last pass");
                free(bank);
            }
        }
        _check(level_loading_screen(), "the letters phase follows");
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
            _check(bank && sz >= 0x400u + 0x19u * 0x180u,
                   "LOADING.APL readable");
            if (bank) {
                /* the state after one tick: the "L" at (0xB0, 0x1C) - the
                 * template's y=0x1A plus one dy step.  The D-111 tick above
                 * is the walkers' 0xA0th, so the letters still get 0x20. */
                for (uint32_t k = 0; k < 0x20u; k++) level_loading_tick();
                const uint8_t *page = osys_null_page(0);
                const uint8_t *L = bank + 0x400u + 0x11u * 0x180u;
                uint32_t badpx = 0;
                for (uint32_t r = 0; r < 24u; r++)
                    for (uint32_t c = 0; c < 16u; c++)
                        if (L[r * 16u + c] &&
                            page[(0x24u + r) * 320u + 0x60u + c] !=
                                L[r * 16u + c])
                            badpx++;
                _check(badpx == 0u,
                       "the 'L' block drawn at x=0x60 (_bad chars)");
                free(bank);
            }
        }
        for (uint32_t t = 0; t < 0x60u; t++) level_loading_tick();
        _check(!level_loading_screen(), "the LOADING screen finishes");
        if (level_load(0) == 0 && !level_loading_screen()) {
            /* a fresh load starts again with the chain's fades to black */
            level_begin();
            for (uint32_t t = 0; t < 400u && !level_loading_screen(); t++) {
                level_begin();
                level_tick();
                palette_tick();
            }
            _check(level_loading_screen(), "a reload restarts the screen");
            for (uint32_t t = 0; t < 0x100u; t++) level_loading_tick();
        }
    } else printf("skip: the level data is not on the disc\n");

    /* 4. the level's own end: the stub hook + [0x138E]'s outcome */
    if (level_load(0) == 0) {
        _check(!level_ended(), "a fresh level is in play");
        level_request_end(1);
        _check(level_ended() && level_outcome(), "won: [0x138E] stays 0xFF");
        _check(level_load(0) == 0 && !level_ended(),
               "a reload clears the end flag ([0x138E] := 0xFF at 3068)");
        level_request_end(0);
        _check(level_ended() && !level_outcome(), "extinct: [0x138E] = 0");

        /* 4. the start snapshot [0x1970] and level_restore's reset */
        {
            uint16_t req = level_required(), mem = level_members();
            const uint8_t *map = level_map();
            uint32_t counted = 0;
            for (uint32_t i = 0; i < 640u; i++)
                if (map[i] == 0xAAu) counted++;
            _check(req == counted, "required = the map's 0xAA count");
            _check(mem == 12u, "the fresh board's 12 members");
            level_restore_members();
            _check(level_members() == mem && level_required() == req,
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
            _check(memcmp(base, won, sizeof base) != 0, "the end page draws");
            _check(memcmp(won, gone, sizeof won) != 0,
                   "the two end texts differ");
            /* pixel-exact: textpage_draw's line is a 16-row cell at
             * top+15 = 67 for a one-line page; nothing else may change */
            {
                uint32_t bad = 0, ink = 0;

                /* one line -> rows = 3, so the published BOX.SPR 9-slice
                 * spans top..top+3*16-1 = 52..99 and the text row is 67 */
                for (uint32_t i = 0; i < sizeof won; i++)
                    if (won[i] != base[i]) {
                        uint32_t y = i / 320u;
                        if (y < 52u || y > 99u) bad++;
                        if (y == 67u) ink++;
                    }
                _check(bad == 0u && ink != 0u,
                       "the end page stays inside its 3-row box (52..99)");
            }
        }
    } else {
        printf("skip: the level data is not on the disc\n");
    }

    /* 6. the level-end trigger (object_test_dispatch 1000:35D6): the goal
     * hook wins; losing the whole tribe ends the level with [0x138E]=0 */
    if (level_load(0) == 0) {
        level_goal_reached();
        _check(level_end_test() && level_ended() && level_outcome(),
               "the goal hook ends the level as won");
        /* check_highscore's score (1000:41AE): [0x1F41]*[0x11FC]*10 (55EA)
         * + the remaining seconds*10 (569B..56CB) */
        {
            statusbar_start(0x0600u, 12u, 4u); /* 6:00, 12 members */
            _check(score_level_score() == 1u * 12u * 10u + 360u * 10u,
                   "the score: level 1 x 12 members + 6:00 left = 3720");
            statusbar_start(0x0530u, 12u, 4u); /* 5:30 left */
            _check(score_level_score() == 1u * 12u * 10u + 330u * 10u,
                   "the score follows the remaining time (5:30 = 3420)");
        }
        _check(level_load(0) == 0 && !level_ended(),
               "a reload clears the end trigger");
        {
            uint16_t n = level_members();
            for (uint32_t i = 0; i < n; i++) level_member_lost();
            _check(level_members() == 0u, "the tribe count reaches zero");
            _check(level_end_test() && level_ended() && !level_outcome(),
                   "an extinct tribe ends the level as lost");
            _check(score_level_score() == 0u,
                   "a lost level adds no score (55B3 -> 578A)");
        }
        (void)level_load(0);
    } else printf("skip: the level data is not on the disc\n");

    /* the 0xC0 tile sets [0x180A] (1000:5535/0x5538), the record whose op 9
     * takes the claim branch.  Level 38 carries a 0xC0; level 27 does not. */
    {
        if (level_load(37u) == 0) { /* 0-based 37 = level 38 */
            int32_t p = object_player();

            _check(p >= 0 && (uint32_t)p < object_static_slots(),
                   "the 0xC0 human becomes the player record");
            if (p >= 0 && (uint32_t)p < object_static_slots()) {
                uint8_t *pr = object_slot((uint32_t)p);

                _check(rd_le_u16(pr + 4) == 0x0010u &&
                           rd_le_u16(pr + 8) == 0x0002u,
                       "the player record is a placed human");
            }
            {
                uint32_t n = 0;

                object_set_action_entity(0u);
                while (object_action_entity() != (uint32_t)p && n++ < 16u)
                    (void)object_cycle_entity(1);
                _check(object_action_entity() == (uint32_t)p &&
                           object_action_program() == 5u &&
                           object_action_byte(1u + object_action_byte(1u)) ==
                               9u,
                       "selecting the 0xC0 human gives program 5, op 9");
            }
        } else printf("skip: level 38 is not on the disc\n");
        if (level_load(26u) == 0) { /* 0-based 26 = level 27, no 0xC0 */
            _check(object_player() == -1,
                   "a level without a 0xC0 tile has no player record");
            /* the torch ignite (1000:39E4 -> 0xEF0C/0xEF39): a walking human
             * (type 0/1) holding the torch burns a placed record within 0x10.
             * 0xEF33/0xEF60 call object_find_overlap in BOTH the found-bush
             * and no-bush paths, so the human is hit with no bush in front. */
            {
                uint8_t *a = object_slot(0u), *v = object_slot(1u);

                a[0] = 0x00;
                a[1] = 0x01;
                a[2] = 0x80;
                a[3] = 0x00;
                a[6] = 0x00;
                a[7] = 0x01; /* carrying the torch */
                a[8] = 0x02;
                a[9] = 0x00;
                v[0] = 0x10;
                v[1] = 0x01;
                v[2] = 0x80;
                v[3] = 0x00;
                v[6] = 0x00;
                v[7] = 0x00;
                v[8] = 0x02;
                v[9] = 0x00;
                object_set_slot_class(1u, 0x86u); /* the carried torch */
                object_set_action_entity(0u);
                /* one LEFT frame leaves the human in the walking type 1 */
                (void)object_command_run(a, 0x04);
                _check(rd_le_u16(a + 8u) < 2u,
                       "the walk leaves the actor type 0/1");
                object_action_set_program(4u);
                object_action_set_cursor(3u); /* program 4: op 3,5,9,6 */
                object_action_run(); /* op 9 = ignite */
                _check(rd_le_u16(v + 4u) == 0x0103u &&
                           rd_le_u16(v + 8u) == 0x001Eu,
                       "the torch ignite burns the nearby human");
                _check(!object_claim_active(),
                       "the ignite is not the [0x180A] claim");
            }
        }
        (void)level_load(0); /* restore the level the blocks below expect */
    }

    /* 6b. the placed-object array (0x3E75) + object_test_dispatch's
     * predicates (1000:35D6) and the claim's writes (1000:3A6A) */
    {
        uint8_t *r;
        static uint8_t testPal[768]; /* the level palette the fades return to */
        object_init();
        _check(
            object_static_slots() == 8u && object_slots() == 29u &&
                object_live_count() == 8u && object_current() == -1,
            "the placed array starts at 8 used (load_terrain_b) with none current");
        _check(object_none_type1d() == -1, "no 0x1D record -> true");
        _check(object_type_equals(3u) == 0, "record 0 is not type 3");

        /* the player at (0x100, 0x80); a neighbour 0x20 right, 8 down, type 2 */
        object_set_player(0);
        r = object_slot(0);
        r[0] = 0x00;
        r[1] = 0x01;
        r[2] = 0x80;
        r[3] = 0x00;
        r[6] = 0x00; /* active */
        object_set_current(0);
        object_set_action_entity(0u);
        _check(object_find_neighbour(0) == -1,
               "an empty array has no neighbour");

        r = object_slot(3);
        r[0] = 0x20;
        r[1] = 0x01;
        r[2] = 0x88;
        r[3] = 0x00; /* +0x20, +8 */
        r[6] = 0x00; /* active    */
        r[8] = 0x02; /* type 2    */
        _check(object_find_neighbour(0) == 3, "the window finds the neighbour");
        _check(object_type_equals(2u) == 0,
               "the predicate reads the *acting* record");
        object_set_action_entity(3u);
        _check(object_type_equals(2u) == -1, "the neighbour's type matches");
        _check(object_class_matches(0x15u) == 0, "its class is 0");
        object_set_action_entity(0u);

        /* D-175: `[0x3E5E + rec[+7]]` is a byte of the data segment, not the
         * 0x40-byte class array.  0x3E5E + 0x15 = 0x3E73 (the live count),
         * 0x3E5E + 0x99 = 0x3EF7 (record 7's +4). */
        {
            uint8_t *a = object_slot(0), *w = object_slot(7u);

            a[7] = 0x15u;
            _check(object_class_matches(object_live_count() & 0x3Fu) == -1,
                   "class byte 0x15 reads the live count (0x3E73)");
            a[7] = 0x99u;
            w[4] = 0x05u;
            _check(object_class_matches(5u) == -1,
                   "class byte 0x99 reads record 7's +4 (0x3EF7)");
            w[4] = 0u;
            a[7] = 0u;
        }

        /* out of the window on either axis, inactive, or type >= 3: no claim */
        r[8] = 0x03;
        _check(object_find_neighbour(0) == -1, "a type >= 3 record is skipped");
        r[8] = 0x00;
        r[6] = 0x80; /* inactive  */
        _check(object_find_neighbour(0) == -1, "an inactive record is skipped");
        r[6] = 0x00;
        r[0] = 0x22;
        r[1] = 0x01; /* +0x22     */
        _check(object_find_neighbour(0) == -1, "0x22 is outside |dx| <= 0x20");
        r[0] = 0x20;
        r[1] = 0x01;
        r[2] = 0x90;
        r[3] = 0x00; /* +0x10     */
        _check(object_find_neighbour(0) == -1, "0x10 is outside |dy| <= 8");
        r[2] = 0x88;
        r[3] = 0x00;

        _check(object_none_type1d() == -1, "still no 0x1D record");
        r[8] = 0x1d;
        _check(object_none_type1d() == 0, "a 0x1D record falsifies it");
        r[8] = 0x02;

        /* the claim (1000:3A6A): the gate, the scan, the writes and then the
         * whole sequence - red/white flash, blinks, the animation, the spawn
         * and the settle - driven with object_tick + palette_tick */

        /* the window is not 4-aligned on the way in: 1000:3B28 snaps it */
        level_set_view(18u, 8u);
        _check(object_claim_begin(0u, 0) == 3, "the claim takes the neighbour");
        _check(r[4] == 0x03 && r[5] == 0x01,
               "the neighbour's frame becomes 0x103");
        _check(r[6] == 0 && r[7] == 0, "the neighbour's flags word clears");
        _check(object_slot(0)[4] == 0x20 && object_slot(0)[5] == 0,
               "the old current object's frame becomes 0x20");
        _check(object_current() == 3, "the claim makes the neighbour current");
        _check(object_claim_active() == 1, "the sequence starts");
        uint32_t spawned = 0;
        {
            uint32_t t = 0, moved = 0, sparkSeen = 0;
            uint8_t frozen = 0, sparkOK = 1;

            /* 1000:3B19/3B28: the sequence freezes the world and snaps the
             * window to 4 px; nothing in it writes the window (the 3D41/3DC0
             * pair is a strip SAVE), so the camera must not move. */
            _check(level_view_x() == 16u && level_view_y() == 8u,
                   "the claim snaps the window to 4 px, without moving it");
            while (object_claim_active() && t++ < 20000u) {
                palette_tick();
                object_tick(testPal);
                level_claim_frame(); /* the claim's presented frame */
                if (!level_updates()) frozen = 1;
                if (level_view_x() != 16u || level_view_y() != 8u) moved = 1;
                if (!sparkSeen && level_spark_active()) {
                    uint32_t sx = 0, sy = 0, sv = 0;

                    if (level_spark_peek(&sx, &sy, &sv) && sv != 0u) {
                        uint32_t px = sx - level_view_x();
                        uint32_t py = sy - level_view_y();
                        const uint8_t *pg = osys_null_page(0);

                        if (px < 320u && py < 200u) {
                            sparkSeen = 1;
                            /* D-199 (closed 2026-11-05): 1000:3F9C writes the
                             * whole shade byte into the pixel's plane, so the
                             * pixel carries all 8 bits, not one scattered bit
                             * over each of the group's four pixels. */
                            sparkOK = (pg[py * 320u + px] == (uint8_t)sv);
                        }
                    }
                }
            }
            _check(frozen, "the claim freezes update_world ([0xC] = 0)");
            _check(!moved, "the claim never moves the window");
            _check(level_updates(), "the claim restores [0xC] (1000:3CAB)");
            _check(!object_claim_active(), "the claim sequence finishes");
            _check(sparkSeen != 0u, "a spawned cell carries a sprite value");
            _check(sparkOK != 0u,
                   "D-199: the composed pixel is the cell's full shade");
            _check(!level_spark_active(),
                   "the reveal finishes before the claim ends (1000:3C73)");
            /* the level's start chain still has to run in play: the later
             * fixtures need the PLAYING phase and a composed page. */
            if (level_starting()) {
                for (uint32_t i = 0; i < 1200u && level_starting(); i++) {
                    level_begin(); /* waits for a running fade */
                    level_tick();
                    palette_tick();
                    if (level_loading_screen()) level_loading_tick();
                }
            }
            /* D-132 (closed 2026-11-05): the claim's scratch strip.  1000:4073
             * paints an expired cell's plane byte into the 0x12 x 0x40 strip
             * that 1000:3DC0 saved, and 1000:3E29 restores it over the page
             * before every frame, so the pixel outlives the particle. */
            {
                const uint8_t *page = osys_null_page(0);
                uint8_t sh[256];

                uint32_t ox = level_view_x() + 0x40u;
                uint32_t oy = level_view_y() + 0x40u;

                memset(sh, 0x5Au, sizeof sh);
                level_spark_begin(ox, oy, sh);
                /* entry 1 (offset 8): live, value 0xA5, age 0x52 (one step
                 * from expiry), at (0x70, 0x80) - inside the strip, 0x10 px
                 * left of the spawn cluster. */
                level_spark_set_byte(0x08u, 0xFFu);
                level_spark_set_byte(0x09u, 0xA5u);
                level_spark_set_byte(0x0Au, (uint8_t)(ox - 0x10u));
                level_spark_set_byte(0x0Bu, (uint8_t)((ox - 0x10u) >> 8));
                level_spark_set_byte(0x0Cu, (uint8_t)oy);
                level_spark_set_byte(0x0Du, (uint8_t)(oy >> 8));
                level_spark_set_byte(0x0Eu, 0x52u);
                level_spark_set_byte(0x0Fu, 0x00u);
                level_tick(); /* the step expires it and paints the strip */
                {
                    uint32_t ex = (uint32_t)level_spark_byte(0x0Au) |
                                  ((uint32_t)level_spark_byte(0x0Bu) << 8);
                    uint32_t ey = (uint32_t)level_spark_byte(0x0Cu) |
                                  ((uint32_t)level_spark_byte(0x0Du) << 8);
                    int32_t col = ((int32_t)ex - ((int32_t)ox - 0x20)) >> 2;
                    int32_t row = (int32_t)ey - ((int32_t)oy - 0x20);
                    int32_t plane = ((int32_t)ex - ((int32_t)ox - 0x20)) & 3;
                    uint32_t sx = ex - level_view_x();
                    uint32_t sy = ey - level_view_y();

                    _check(level_spark_byte(0x08u) == 0u,
                           "D-132: the planted cell expires");
                    _check(col >= 0 && col < 0x12 && row >= 0 && row < 0x40 &&
                               level_spark_strip_at((uint32_t)col,
                                                    (uint32_t)row,
                                                    (uint32_t)plane) == 0xA5u,
                           "D-132: the expired cell paints the strip");
                    level_tick(); /* the restore puts it on the page */
                    _check(sx < 320u && sy < 192u &&
                               page[sy * 320u + sx] == 0xA5u,
                           "D-132: the strip restores it over the page");
                }
            }
            spawned = object_live_count();
            _check(spawned == 9u, "the spawn appended one live record");
            if (spawned == 9u) {
                /* 0x3F17 + 0x12*(8-8) = port index 9, class 8-7 = 1 */
                const uint8_t *nr = object_slot(9);
                _check((rd_le_u16(nr + 0)) == 0x0120u,
                       "the spawned record takes the neighbour's x");
                _check((rd_le_u16(nr + 2)) == 0x88u + 9u,
                       "the spawned record sits 9 below the neighbour");
                _check((rd_le_u16(nr + 4)) == 0x0060u,
                       "its frame is the recipe's 0x60 (the spear)");
                _check((rd_le_u16(nr + 8)) == 0xFFFFu,
                       "its +8 is the recipe's first parameter");
                _check(object_slot_class(1u) == 5u,
                       "its behaviour 5 lands in class slot 1");
            }
            _check(r[6] & 0x80, "the settle step removes the neighbour");
            /* the animation's six 4FE9 increments over its 6 x 9 frames */
            _check((rd_le_u16(r + 4)) == 0x0109u,
                   "the neighbour animates 0x103 -> 0x109");

            /* the [0x180A] claim (1000:39E4): for the player's own record
             * (SI == [0x180A]) the op does MOV AX,2 and jumps to 0x3A6A, so
             * the claim appends the 0x9C torch, not the spear.  The spear's
             * claim above removed the neighbour, so re-arm it first. */
            r[6] = 0x00;
            r[7] = 0x00;
            r[8] = 0x02;
            r[4] = 0x10;
            r[5] = 0x00;
            object_action_set_program(4u);
            object_action_set_cursor(3u); /* program 4: op 3,5,9,6 */
            object_action_run();
            _check(object_claim_active(),
                   "the player's op 9 starts the 0x9C claim");
            {
                uint32_t t3 = 0;

                while (object_claim_active() && t3++ < 20000u) {
                    palette_tick();
                    object_tick(testPal);
                    level_claim_frame(); /* the reveal's presented frame */
                }
            }
            _check(!object_claim_active(), "the 0x9C claim sequence finishes");
            _check(r[6] & 0x80, "the [0x180A] claim removes the neighbour");
            _check(object_live_count() == 10u, "the claim appends one record");
            {
                const uint8_t *nr = object_slot(10);

                _check(rd_le_u16(nr + 0) == 0x0120u &&
                           rd_le_u16(nr + 2) == 0x0091u,
                       "the torch lands at the neighbour's feet");
                _check(rd_le_u16(nr + 4) == 0x009Cu,
                       "the claim's sprite is the 0x9C torch");
                _check(rd_le_u16(nr + 8) == 0xFFFFu,
                       "the torch's +8 is the recipe's first parameter");
                _check(object_slot_class(2u) == 6u,
                       "the torch's behaviour 6 lands in class slot 2");
            }
            /* D-240: the claim's pan recolour freezes the black page until
             * 1000:3CB5's re-blit.  The claim above was driven with
             * object_tick, which composes nothing; this one goes through
             * level_tick, where the page is built. */
            if (!level_starting()) {
                uint8_t *pr = object_slot(0u), *nb = object_slot(3u);
                const uint8_t *pg = osys_null_page(0);
                uint32_t t4 = 0, pan = 0, afterPan = 0;
                uint32_t parity = object_frame_counter_probe() & 1u;
                uint32_t vx0 = level_view_x(), vy0 = level_view_y();
                int32_t black = 1, overBlack = 0, restored = 0;
                int32_t panInk = 0, panKept = 0, keptDone = 0;
                uint32_t witnessPix = 0;
                uint8_t witnessInk = 0;
                int32_t witnessFound = 0, settleInk = 1, settleDone = 0;

                object_init();
                /* the torch claim above was driven with object_tick, so its
                 * particle strip is still live; drain it on a clean array
                 * before the page-freeze test (the real game never starts a
                 * second claim with a reveal still running). */
                while (level_spark_active() && t4++ < 400u) {
                    level_tick();
                    palette_tick();
                }
                t4 = 0u;
                object_set_player(0);
                object_set_action_entity(0u);
                pr[0] = 0x00;
                pr[1] = 0x01;
                pr[2] = 0x80;
                pr[3] = 0x00;
                pr[6] = 0x00;
                pr[8] = 0x00;
                nb[0] = 0x20;
                nb[1] = 0x01;
                nb[2] = 0x88;
                nb[3] = 0x00;
                nb[6] = 0x00;
                nb[8] = 0x02;
                /* D-242: the pre-pan red/white phases must leave the last
                 * drawn page - the sprite layer included - untouched.  Record
                 * 5 is a witness sprite away from the claim's pair. */
                {
                    static uint8_t snap[320u * 200u];
                    uint8_t *wit = object_slot(5u);
                    uint32_t avx = level_view_x() & ~3u;
                    uint32_t avy = level_view_y();

                    wit[0] = (uint8_t)(avx + 0x20u);
                    wit[1] = 0u;
                    wit[2] = (uint8_t)(avy + 0x20u);
                    wit[3] = 0u;
                    wit[4] = 0x10;
                    wit[6] = 0x00;
                    wit[8] = 0x02;
                    level_set_view(avx, avy);
                    level_redraw_page();
                    memcpy(snap, pg, sizeof snap);
                    wit[6] = 0x80u; /* hide the witness */
                    level_redraw_page();
                    for (uint32_t i = 0; i < 320u * 200u; i++)
                        if (pg[i] != snap[i]) {
                            witnessPix = i;
                            witnessInk = snap[i];
                            witnessFound = 1;
                            break;
                        }
                    wit[6] = 0x00u; /* restore it */
                    level_redraw_page();
                    _check(witnessFound,
                           "D-242: the witness sprite draws on the world page");
                    _check(object_claim_begin(0u, 0) == 3,
                           "D-240: a fresh claim takes the neighbour");
                    level_tick(); /* the first pre-pan claim frame */
                    _check(witnessFound && pg[witnessPix] == witnessInk,
                           "D-242: the pre-pan flash keeps the sprites");
                }
                while (object_claim_active() && t4++ < 4000u) {
                    level_tick();
                    palette_tick();
                    if (object_claim_pan_active()) {
                        pan = 1u;
                        afterPan = 0u;
                        continue;
                    }
                    if (pan && !afterPan) {
                        uint32_t i, ink = 0;

                        afterPan = 1u;
                        /* 1000:3D5E: the world bytes below 0x80 are 0x1E; the
                         * two records 1000:3D30 draws after it are excluded. */
                        for (i = 0; i < 320u * 200u; i++)
                            if (pg[i] < 0x80u && pg[i] != 0x1Eu &&
                                !_flow_claim_rec_box(i % 320u, i / 320u,
                                                     level_view_x(),
                                                     level_view_y())) {
                                black = 0;
                                break;
                            }
                        /* D-243: 40CA draws the victim (frame 0x103, the
                         * surprised face) on the recoloured page. */
                        for (i = 0; i < 320u * 200u; i++)
                            if (_flow_claim_rec_box(i % 320u, i / 320u,
                                                    level_view_x(),
                                                    level_view_y()) &&
                                pg[i] < 0x80u && pg[i] != 0x1Eu)
                                ink++;
                        panInk = ink != 0u;
                    } else if (pan && !keptDone &&
                               !object_claim_anim_active()) {
                        uint32_t i, ink = 0;

                        keptDone = 1;
                        for (i = 0; i < 320u * 200u; i++)
                            if (_flow_claim_rec_box(i % 320u, i / 320u,
                                                    level_view_x(),
                                                    level_view_y()) &&
                                pg[i] < 0x80u && pg[i] != 0x1Eu)
                                ink++;
                        panKept = ink != 0u;
                    }
                    if (!settleDone && witnessFound &&
                        object_claim_settle_active()) {
                        settleDone = 1;
                        settleInk = (pg[witnessPix] == witnessInk);
                    }
                    if (object_claim_anim_active()) {
                        uint32_t i;

                        /* 1000:40CA draws the selected + claimed records on it. */
                        for (i = 0; i < 320u * 200u; i++)
                            if (pg[i] < 0x80u && pg[i] != 0x1Eu) {
                                overBlack = 1;
                                break;
                            }
                    }
                }
                _check(pan, "D-240: the claim reaches its pan");
                _check(black,
                       "D-240: the foreground stays black after the pan");
                _check(panInk,
                       "D-243: the pan draws the victim over the black page");
                _check(
                    panKept,
                    "D-243: the pan's records persist into the frozen frames");
                _check(overBlack,
                       "D-240: the two records draw over the black page");
                _check(settleDone && settleInk,
                       "D-244: the settle page keeps the level's sprites");
                _check(!object_claim_active(), "D-240: the claim finishes");
                {
                    uint32_t i;

                    for (i = 0; i < 320u * 200u; i++)
                        if (pg[i] < 0x80u && pg[i] != 0x1Eu) {
                            restored = 1;
                            break;
                        }
                }
                _check(restored,
                       "D-240: the world is re-blitted after the claim");
                level_set_view(vx0, vy0);
                /* the later mover fixtures gate on [0x4103] parity: keep the
                 * frame counter's phase where this block found it. */
                if ((object_frame_counter_probe() & 1u) != parity)
                    object_claim_frame(level_palette());
            }
            /* D-48: 1000:34E6 - the play loop's exit.  The tick that detects
             * the end plays sound 0xC (1000:83B7) and freezes the world for
             * the 15 x 1000:8B56 waits before draw_level_text's chain. */
            if (!level_starting()) {
                uint32_t h;

                level_request_end(1);
                level_tick();
                _check(level_end_holding() && !level_ended(),
                       "1000:34E6: the exit holds the frozen world");
                _check(object_last_sfx() == 0x0Cu,
                       "1000:34E6: the exit plays sound 0xC");
                for (h = 0; h < 15u; h++) {
                    _check(level_end_holding(), "1000:34E6: 15 waits");
                    level_tick();
                }
                _check(!level_end_holding() && level_ended(),
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
        _check(level_data() && level_data()->objectTest == 0x0502u,
               "level 1's goal test word is 0x0502 (mode 2, arg 5)");
        _check(object_test_dispatch(0x0502u) == 0,
               "an empty array does not meet it");
        object_set_current(0);
        object_set_slot_class(1u, 5u);
        object_slot(0)[7] = 1u; /* the record's source slot */
        _check(object_test_dispatch(0x0502u) == 1,
               "class 5 in slot 1 meets it");
        object_set_slot_class(1u, 4u);
        _check(object_test_dispatch(0x0502u) == 0, "class 4 does not");
        object_set_slot_class(1u, 5u);
        /* level_end_test is object_test_dispatch now */
        (void)level_load(0);
        object_set_current(0);
        object_set_slot_class(1u, 5u);
        object_slot(0)[7] = 1u;
        _check(level_end_test() && level_outcome(),
               "the level's own goal test ends it as won");

        /* 0x36AB: the map cell under the record (the helper is 0xF761, the
         * map address, not a random generator) */
        object_init();
        (void)level_load(0);
        object_set_current(0);
        r = object_slot(0);
        r[0] = 0x20;
        r[1] = 0x00; /* x = 0x20          */
        r[2] = 0x10;
        r[3] = 0x00; /* y = 0x10          */
        r[8] = 0x02; /* type 2            */
        {
            /* cell = 32*((0x10+0xC)/16) + (0x20+8)/16 = 32*1 + 2 = 34 */
            uint32_t cell = level_map()[34];
            _check(object_map_cell_equals(cell) == -1,
                   "the map cell under the record matches");
            _check(object_map_cell_equals(cell + 1u) == 0,
                   "a different cell does not");
        }
        r[8] = 0x03;
        _check(object_map_cell_equals(level_map()[34]) == 0,
               "a type > 2 record is not tested against the map");
        r[8] = 0x02;
        _check(object_test_dispatch(0x0005u | (level_map()[34] << 8)) == 1,
               "mode 5 dispatches to the map cell");
        object_init();
    }

    /* 6c. load_terrain_b's object walk (1000:5168): level 1's four 0xAA cells
     * become the humans in static slots 0..3 and its one 0xAB cell (28,2) the
     * spear appended at index 9 - the live [0x3E73] = 9 / [0x3E5F] = 5 state */
    if (level_load(0) == 0) {
        const uint8_t *sp = object_slot(9);
        _check(object_live_count() == 9u && object_placed_humans() == 4u,
               "the walk leaves 9 used records and counts 4 humans");
        /* 0xF761's cell mapping (the score's object bonus reads it): the first
         * human sits one row above the 0xAA marker at (3,2), so the cell under
         * it -- 32*((24+0xC)/16) + (48+8)/16 -- is that 0xAA */
        _check(object_record_map_cell(0u) == 0xAAu,
               "the map cell under a record is the 0xAA marker");
        _check((rd_le_u16(sp + 0)) == 28u * 16u &&
                   (rd_le_u16(sp + 2)) == 2u * 16u,
               "the 0xAB cell at (28,2) appends at (448,32)");
        _check((rd_le_u16(sp + 4)) == 0x0060u && (rd_le_u16(sp + 8)) == 0xFFFFu,
               "the spear's frame 0x60 and +8 = 0xFFFF");
        _check(object_slot_class(1u) == 5u,
               "its behaviour 5 lands in class slot 1");
        /* D-161: the unbounded word[0x5BA4 + 2*type] read: the real entries
         * 0..2, the 0x5BAA frame table 5/6, and type 0x0F in the pause text. */
        _check(object_throw_dx_for_probe(0u) == 8 &&
                   object_throw_dx_for_probe(1u) == -8 &&
                   object_throw_dx_for_probe(2u) == 0 &&
                   (uint32_t)object_throw_dx_for_probe(5u) == 0x6000u &&
                   (uint16_t)object_throw_dx_for_probe(6u) == 0x969Cu &&
                   object_throw_dx_for_probe(0x0Fu) == 0x2045,
               "the throw reads the words past 0x5BA4 (D-161)");
        /* D-162: the fourth recipe's params are 0/0/0xFFFF/0xFFFF. */
        _check(object_recipe_param_for_probe(3u, 0u) == 0u &&
                   object_recipe_param_for_probe(3u, 1u) == 0u &&
                   object_recipe_param_for_probe(3u, 2u) == 0xFFFFu &&
                   object_recipe_param_for_probe(3u, 3u) == 0xFFFFu,
               "recipe 3's two 0xFFFF words are p3/p4 (D-162)");
        /* D-103: the pending-move table has 17 real pairs; the last is (2,4)
         * before the 0xFFFF sentinel. */
        _check(object_pend_off_for_probe(16u) == 2u &&
                   object_pend_off_for_probe(17u) == 0xFFFFu &&
                   object_pend_wait_for_probe(16u) == 4u,
               "the pending-move table has 17 real pairs (D-103)");
    }

    /* 6c2. the ACTION program machine (script_op_dispatch 1000:3979): level 1's
     * win is the pickup - the collision pass finds the spear, marks its slot
     * 0x85 and writes the live ordinal as the actor's class, which the goal
     * word 0x0502 (class match, argument 5) then accepts */
    if (level_load(0) == 0) {
        uint8_t *actor, *spear;

        object_set_action_entity(0u);
        actor = object_slot(0);
        spear = object_slot(9);
        _check(spear != NULL && (rd_le_u16(spear + 4)) == 0x0060u,
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
        _check(object_test_dispatch(0x0502u) == 0,
               "the goal is not met before the pickup");
        object_action_run();
        _check((actor[7]) == 1u, "the pickup writes the ordinal as the class");
        _check(object_slot_class(1u) == 0x85u,
               "the spear's slot is marked taken (0x85)");
        _check(object_test_dispatch(0x0502u) == 1,
               "level 1's goal (class match, arg 5) is met after the pickup");
        /* op 4 (0xEFA1): the record's state byte must be 1 (behaviour 5, the
         * spear, whose slot 5 maps to state 1 in 0x6EB6); the pickup left
         * [0x5B56] = 2 (0x5B8A[5]), and program 2's prog[6] is op 4, so the
         * cursor at 5 selects it */
        _check(object_action_program() == 2u, "the pickup left program 2");
        object_action_set_cursor(5u); /* program 2: prog[6] = op 4  */
        actor[8] = 0x00;
        actor[9] = 0x00; /* type <= 2 gates the op      */
        object_action_run();
        _check((rd_le_u16(actor + 4)) == 0x00BBu &&
                   (rd_le_u16(actor + 8)) == 0x0017u,
               "op 4 sets the fixed state (frame 0xBB, type 0x17)");
        object_action_set_cursor(1u);

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
            uint32_t i, _bad = 0u;

            for (i = 0; i < 0x15u; i++)
                if (object_beh_state_for_probe(i) != want[i]) _bad++;
            _check(_bad == 0u, "0x6EB6: the 21 state entries are the image's");
            _check(object_beh_state_for_probe(8u) == 0x0Fu &&
                       object_beh_state_for_probe(16u) == 0x01u &&
                       object_beh_state_for_probe(20u) == 0x01u,
                   "0x6EB6: behaviours 8 and 16..20 are in the window");
            _check(object_beh_state_for_probe(0x15u) == 0x00u &&
                       object_beh_state_for_probe(0x16u) == 0x01u,
                   "0x6EB6: the window continues into objTypeOverlaps[8..]");
        }
        /* the same table through op 4 (1000:EFA1's `objStateIs(actor, 1)`):
         * behaviour 16's state is 1, so the pose is applied; behaviour 13's
         * is 0, so it is not.  (Before D-160's fix both read past the 8-byte
         * array into whatever followed.) */
        object_set_slot_class(1u, 16u);
        actor[4] = 0x10;
        actor[5] = 0;
        actor[8] = 0;
        actor[9] = 0;
        object_action_set_cursor(5u);
        object_action_run();
        _check((rd_le_u16(actor + 4)) == 0x00BBu &&
                   (rd_le_u16(actor + 8)) == 0x0017u,
               "op 4 applies for behaviour 16 (state 1)");
        object_set_slot_class(1u, 13u);
        actor[4] = 0x10;
        actor[5] = 0;
        actor[8] = 0;
        actor[9] = 0;
        object_action_set_cursor(5u);
        object_action_run();
        _check((rd_le_u16(actor + 4)) == 0x0010u &&
                   (rd_le_u16(actor + 8)) == 0x0000u,
               "op 4 does not apply for behaviour 13 (state 0)");
        object_action_set_cursor(1u);
        object_set_slot_class(1u, 0x85u); /* the spear's taken mark back   */


        /* and the level's own flow ends it as won */
        object_pending_run();
        level_begin();
        for (uint32_t t = 0; t < 4000u && level_starting(); t++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        level_tick();
        /* D-48: 1000:34E6 freezes the world for 15 frames before the caption */
        for (uint32_t h = 0; h < 15u && level_end_holding(); h++) level_tick();
        _check(level_ended() && level_outcome(),
               "the pickup ends level 1 as won");

        /* op 7 (0x39CF): the ride command.  Type 0 -> 0xEA0F (frame 0xAB,
         * +0xA = 3, [0x423C] = 9), type 1 -> 0xEA4D (0xAD/3/0xA); the
         * 0xF2CF state-2 gate and the CMP AX,2 gate apply. */
        {
            uint8_t *a = object_slot(0);

            object_init();
            object_set_action_entity(0u);
            object_set_slot_class(1u, 7u); /* behaviour 7 -> state 2 */
            a[7] = 1u;
            a[0] = 100;
            a[1] = 0;
            a[2] = 100;
            a[3] = 0;
            object_action_set_program(3u);
            object_action_set_cursor(2u); /* prog[3] = op 7 */
            object_set_action_latched(0);
            a[8] = 0;
            a[9] = 0;
            object_action_run();
            _check(rd_le_u16(a + 4) == 0x00AB && rd_le_u16(a + 0xA) == 3,
                   "op 7 type 0 rides 0xEA0F (frame 0xAB)");
            object_set_action_latched(0);
            a[8] = 1;
            a[9] = 0;
            a[4] = 0x10;
            a[0xA] = 0;
            object_action_run();
            _check(rd_le_u16(a + 4) == 0x00AD && rd_le_u16(a + 0xA) == 3,
                   "op 7 type 1 rides 0xEA4D (frame 0xAD)");
            object_set_action_latched(0);
            a[8] = 2;
            a[9] = 0;
            a[4] = 0x10;
            a[0xA] = 0;
            object_action_run();
            _check(rd_le_u16(a + 4) == 0x0010 && rd_le_u16(a + 0xA) == 0,
                   "op 7 is gated out for type 2 (1000:39D2)");
        }

        /* op 9 (0x39E4 -> 0xEF0C) + object_find_overlap (0xF2E0): a behaviour-6
         * actor walks the live record with slot 3 in its upper-left window,
         * marks it type 1, then converts the static record overlapping it */
        if (level_load(0) == 0) {
            uint8_t *mv, *live, *victim;
            object_action_set_program(5u); /* program 5: prog[2] = op 9 */
            object_action_set_cursor(1u);
            object_set_slot_class(2u, 6u); /* class 2 -> behaviour 6    */
            object_set_slot_class(1u, 3u); /* live ordinal 1 -> behav 3 */
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
            _check((rd_le_u16(live + 4)) == 0x0060u,
                   "the live record is the spear");
            object_action_run();
            _check((rd_le_u16(live + 8)) == 0x0001u &&
                       (rd_le_u16(live + 10)) == 0x0003u,
                   "op 9 marks the walked live record as state 1");
            _check((rd_le_u16(victim + 4)) == 0x0103u &&
                       (rd_le_u16(victim + 8)) == 0x001Eu &&
                       (rd_le_u16(victim + 10)) == 0x0006u &&
                       (rd_le_u16(victim + 14)) == 0x000Au,
                   "the overlap fixup converts the overlapping record");
            /* D-113: the non-player arm's gate is CMP AX,2 / JNC.  The walk
             * window for step 0 (types 1/2) is [x-0x10, x), so move the live
             * record there: type 2 must return, type 1 marks it. */
            wr_le_u16(live + 0u, 432u);
            wr_le_u16(live + 8u, 0u);
            wr_le_u16(live + 0xAu, 0u);
            mv[8] = 2;
            object_set_action_latched(0);
            object_action_run();
            _check(rd_le_u16(live + 8u) == 0u && rd_le_u16(live + 0xAu) == 0u,
                   "op 9 is gated out for type 2 (1000:39F3)");
            mv[8] = 1;
            object_set_action_latched(0);
            object_action_run();
            _check(rd_le_u16(live + 8u) == 1u && rd_le_u16(live + 0xAu) == 3u,
                   "op 9 runs for type 1 (1000:39F3)");
        }

        /* op 3 (0xECFE) with its page gate: level 1's cells (22,1) and (23,1)
         * hold ids 0x1E/0x1F (free) while (18,3) is free and its right
         * neighbour (19,3) is a wall (0x07), so a carrier on the first two
         * enters the open state and one before the wall does not.  "free" is
         * the 0x92A0 row's 16 negative bytes (`1000:F123`), built from the
         * atlas (1000:505C) - not the old field-P table. */
        _check(level_page_free(0x1Eu) == 1 && level_page_free(0x1Fu) == 1 &&
                   level_page_free(0x07u) == 0 && level_page_free(0xB8u) == 0,
               "the page gate: free tiles vs walls (and 0xB8, an item tile)");
        {
            uint8_t *mv;
            object_init();
            object_action_set_program(1u);
            object_action_set_cursor(5u); /* prog[6] = 0x03 (op 3)     */
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
            object_action_run();
            _check((rd_le_u16(mv + 4)) == 0x008Cu &&
                       (rd_le_u16(mv + 8)) == 0x0012u,
                   "op 3 moves a carrier on open ground (0x8C/0x12)");
            /* (18,3) is free but its right neighbour (19,3) is a wall */
            object_init();
            object_action_set_program(1u);
            object_action_set_cursor(5u);
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
            object_action_run();
            _check((rd_le_u16(mv + 4)) == 0x0010u &&
                       (rd_le_u16(mv + 8)) == 0x0000u,
                   "op 3 refuses when the neighbour is a wall");
            /* D-234: the right neighbour of the map's last cell (row 19,
             * col 31) is the buffer's gap (tile 0), read at 1000:ED3C with no
             * 640 bound. */
            if (level_map()) {
                uint8_t *m = (uint8_t *)level_map();
                uint8_t old = m[639];

                m[639] = 0u;
                object_init();
                object_action_set_program(1u);
                object_action_set_cursor(5u);
                mv = object_slot(0);
                mv[0] = (uint8_t)(31u * 16u);
                mv[1] = (uint8_t)((31u * 16u) >> 8);
                mv[2] = (uint8_t)(19u * 16u);
                mv[3] = (uint8_t)((19u * 16u) >> 8);
                mv[4] = 0x10;
                mv[6] = 0;
                mv[7] = 1u;
                mv[8] = 0;
                mv[9] = 0;
                _check(level_page_free(0u) == 1,
                       "the gap byte (tile 0) is a free page");
                object_action_run();
                _check(
                    (rd_le_u16(mv + 4)) == 0x008Cu &&
                        (rd_le_u16(mv + 8)) == 0x0012u,
                    "D-234: op 3 reads the gap as the last cell's neighbour");
                m[639] = old;
            }
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
        bank = res_load_chunk("APL\\SPRITES.DAT", &bankSize, NULL);
        if (!bank || bankSize < 0x4800u + 0x180u) {
            fprintf(stderr, "FAIL SPRITES.DAT unreadable\n");
            _bad = 1;
        } else {
            memset(page, 0, sizeof page);
            object_publish_bank(bank, bankSize);
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
                _check(
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
                _check(wrong == 0u,
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
                    _check(bad2 == 0u && page[21u * 320u + 20u] == 0u,
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
                    _check(bad3 == 0u, "frame 0x2B (mode 2) draws 32 px wide");
                }
                /* 1000:6421 -> 1000:C23C: the placed records render in the
                 * partitioned order.  The acting record is last in its class
                 * (the climber over the member it climbs), and the type 6..8
                 * (stack) class is drawn after the live records. */
                {
                    static uint8_t soloA[320u * 200u], soloB[320u * 200u];
                    uint32_t contested = 0, won = 0;

                    object_init();
                    object_publish_bank(bank, bankSize);
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
                    object_publish_bank(bank, bankSize);
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
                    object_publish_bank(bank, bankSize);
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
                    object_set_action_entity(0u); /* the climber */
                    memset(page, 0, sizeof page);
                    object_render(page, 0, 0);
                    for (uint32_t k = 0; k < 320u * 200u; k++) {
                        if (!soloA[k] || !soloB[k] || soloA[k] == soloB[k])
                            continue;
                        contested++;
                        if (page[k] == soloA[k]) won++;
                    }
                    _check(contested > 20u && won == contested,
                           "the acting record draws last in its class");
                }
                {
                    static uint8_t soloA[320u * 200u], soloB[320u * 200u];
                    uint32_t contaminated = 0, bad2 = 0;

                    /* a type-7 member (front list) over a live record 8 */
                    object_init();
                    object_publish_bank(bank, bankSize);
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
                    object_publish_bank(bank, bankSize);
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
                    object_publish_bank(bank, bankSize);
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
                    _check(contaminated > 20u && bad2 == 0u,
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
                    _check(ink > 50u, "a climb frame (0x23) still draws ink");
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
                    c = object_command_select(rec,
                                              0x02); /* bit 1 -> command 4 */
                    _check(c.handler == 0xE609u && c.command == 4u &&
                               c.tail == 0xD9C7u,
                           "type 2 + bit 1 -> 1000:E609, the push's walk-up");
                    rec[8] = 0x08;
                    c = object_command_select(rec,
                                              0x01); /* bit 0 -> command 0 */
                    _check(c.handler == 0xE963u && c.command == 0u,
                           "type 8 + bit 0 -> 1000:E963 (the snap)");
                    rec[8] = 0x0F;
                    c = object_command_select(rec, 0x02);
                    _check(
                        c.handler == 0u && c.tail == 0xDC1Fu,
                        "type 15 + command 4 -> no handler, the row's own tail");
                    /* the one-shot command (key 0x48 = bit 6 + command 8) counts
                     * down from 0x4B0 and turns the record into a human (type 2) */
                    object_init();
                    rec = object_slot(0);
                    rec[8] = 0x00;
                    rec[9] = 0x00;
                    object_set_command_timer(0x3E8u); /* 1000:41A7's arm */
                    for (uint32_t i = 0; i < 1000u; i++)
                        (void)object_command_select(rec, 0x00);
                    _check(rec[8] == 0x02u && rec[9] == 0x00u,
                           "the 0x6DEA one-shot expires to type 2");
                    /* D-75: with the timer at 0 the type-2 idle stream runs
                     * (0xE7C0); its first hold is 200 ticks, then frame 0x12 */
                    rec[4] = 0x10;
                    rec[5] = 0;
                    for (uint32_t i = 0; i < 2u; i++)
                        (void)object_command_run(rec, 0x00u);
                    _check(object_action_pending() == 0u,
                           "the type-2 stream is reached with [0x6DEA] = 0");
                    {
                        uint32_t seen = 0, i;

                        for (i = 0; i < 700u; i++) {
                            uint32_t f;

                            (void)object_command_run(rec, 0x00u);
                            f = rd_le_u16(rec + 4);
                            if (f >= 0x12u && f <= 0x16u) seen |= 1u << f;
                        }
                        _check(
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
                    object_set_action_entity(0u);
                    object_action_set_program(1u); /* prog[3] = op 1 */
                    object_action_set_cursor(2u);
                    object_action_run();
                    _check((rd_le_u16(base + 4)) == 0x0020u &&
                               (rd_le_u16(base + 8)) == 0x0008u,
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
                    object_set_action_entity(1u);
                    _check(object_stack_push(top) == 1,
                           "the push takes the human standing above the base");
                    _check(
                        (rd_le_u16(top + 4)) == 0x0025u &&
                            (rd_le_u16(top + 8)) == 0x0007u &&
                            (rd_le_u16(top + 0xE)) != 0u,
                        "the pushed human is frame 0x25 / type 7 with its slot");
                    /* the pop: a caller x far from the owner truncates the list
                     * at the top member and returns the record below it */
                    _check(object_stack_pop(200u, &below, NULL) == 1 &&
                               below == base,
                           "the pop removes the top member and names the base");
                    _check(object_stack_scan_dissolve(1u) == 0,
                           "the popped record is no longer in any group");
                    /* push again and dissolve: n = 1 restores the member */
                    _check(object_stack_push(top) == 1, "the push works again");
                    _check(
                        object_stack_scan_dissolve(1u) == 1,
                        "the scan finds and dissolves the group holding rec 1");
                    _check(
                        (rd_le_u16(base + 2)) == 92u &&
                            (rd_le_u16(base + 4)) == 0x0010u &&
                            (rd_le_u16(base + 8)) == 0x0002u,
                        "n = 1 stands the base (FAA4: y-8, frame 0x10, type 2)");
                    _check((rd_le_u16(top + 4)) == 0x0010u &&
                               (rd_le_u16(top + 8)) == 0x0003u,
                           "n = 1 resets the top through FADA (type 3)");
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
                    _tf_wr16(b3, 0u, 100);
                    _tf_wr16(b3, 2u, 100);
                    _tf_wr16(b3, 4u, 0x10u);
                    _tf_wr16(b3, 8u, 2u);
                    object_set_action_entity(0u);
                    object_action_set_program(1u);
                    object_action_set_cursor(2u);
                    object_action_run(); /* op 1 */
                    p1 = object_slot(1);
                    memset(p1, 0, 0x12u);
                    _tf_wr16(p1, 0u, 100);
                    _tf_wr16(p1, 2u, 70);
                    _tf_wr16(p1, 4u, 0x10u);
                    _tf_wr16(p1, 8u, 2u);
                    object_set_action_entity(1u);
                    _check(object_stack_push(p1) == 1, "the 3-stack: member 1");
                    p2 = object_slot(2);
                    memset(p2, 0, 0x12u);
                    _tf_wr16(p2, 0u, 100);
                    _tf_wr16(p2, 2u, 52);
                    _tf_wr16(p2, 4u, 0x10u);
                    _tf_wr16(p2, 8u, 2u);
                    object_set_action_entity(2u);
                    _check(object_stack_push(p2) == 1, "the 3-stack: member 2");
                    g = object_group_at(0u);
                    _check(g != NULL && (g[4] & 0x80u) != 0u,
                           "the group entry starts from the 0xFF clear");
                    object_animator_reset();
                    for (uint32_t k = 0; k < 60u; k++) {
                        int32_t x;

                        object_tick(pal3);
                        x = (int32_t)(int16_t)_tf_rd16(b3, 0u);
                        if (x < bMin) bMin = x;
                        if (x > bMax) bMax = x;
                        x = (int32_t)(int16_t)_tf_rd16(p1, 0u);
                        if (x < mMin) mMin = x;
                        if (x > mMax) mMax = x;
                        x = (int32_t)(int16_t)_tf_rd16(p2, 0u);
                        if (x < tMin) tMin = x;
                        if (x > tMax) tMax = x;
                    }
                    _check(bMin == 100 && bMax == 100,
                           "the stack's base stays at the entry x");
                    _check(mMin == 100 && mMax == 100 && tMin == 100 &&
                               tMax == 100,
                           "the 3-stack does not shear while it animates");
                    _check((g[4] & 0x80u) != 0u,
                           "the state byte keeps the 0xF0 mask");
                    _check((g[4] & 0x0Fu) != 0u,
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
                    _tf_wr16(b4, 0u, 100);
                    _tf_wr16(b4, 2u, 100);
                    _tf_wr16(b4, 4u, 0x10u);
                    _tf_wr16(b4, 8u, 2u);
                    object_set_action_entity(0u);
                    object_action_set_program(1u);
                    object_action_set_cursor(2u);
                    object_action_run();
                    m1 = object_slot(1);
                    memset(m1, 0, 0x12u);
                    _tf_wr16(m1, 0u, 100);
                    _tf_wr16(m1, 2u, 70);
                    _tf_wr16(m1, 4u, 0x10u);
                    _tf_wr16(m1, 8u, 2u);
                    object_set_action_entity(1u);
                    _check(object_stack_push(m1) == 1,
                           "the column for the leave");
                    top = object_slot(2);
                    memset(top, 0, 0x12u);
                    _tf_wr16(top, 0u, 100);
                    _tf_wr16(top, 2u, 52);
                    _tf_wr16(top, 4u, 0x10u);
                    _tf_wr16(top, 8u, 2u);
                    object_set_action_entity(2u);
                    _check(object_stack_push(top) == 1,
                           "the third joins the column");
                    for (uint32_t k = 0; k < 20u; k++) {
                        uint32_t f;

                        (void)object_command_run(top, 0x01u);
                        f = (uint32_t)(rd_le_u16(top + 4));
                        if (f < 0x23u || f > 0x25u) outside = 1;
                        seen |= 1u << (f & 0x1Fu);
                    }
                    _check(!outside && (seen & (1u << 3)) != 0u &&
                               (seen & (1u << 4)) != 0u &&
                               (seen & (1u << 5)) != 0u,
                           "1000:E934: a member climbs down 0x23..0x25");
                    /* the push lifted it from 52 to 53 (1000:E6DA), then one
                     * pixel per press */
                    _check(_tf_rd16(top, 2u) == 52u + 1u + 20u,
                           "1000:E934: the down stepper moves y +1 a press");
                    _tf_wr16(top, 2u, 100u);
                    _check(object_stack_leave(top) == 1 &&
                               _tf_rd16(top, 8u) == 2u &&
                               (rd_le_u16(top + 4)) == 0x10u,
                           "the leave at the entry's y restores the human");
                }

                /* The other four verbs: grab (1000:E58B, with its free-slot
                 * registration), step off (1000:E860), leave (1000:E8DD),
                 * snap (1000:E963) and the selector (1000:F17C).  The 0x64AD /
                 * 0x64C1 masks are written by the pushes and read by the
                 * selector's 0x6F22 _check. */
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
                    object_set_action_entity(0u);
                    object_action_set_program(1u);
                    object_action_set_cursor(2u);
                    object_action_run(); /* op 1: the base */
                    top = object_slot(1);
                    top[0] = 102;
                    top[1] = 0;
                    top[2] = 101;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_set_action_entity(1u);
                    _check(object_stack_grab(top) == 1,
                           "the grab takes a human within 8 x / 4 y");
                    _check(
                        (rd_le_u16(top + 4)) == 0x0023u &&
                            (rd_le_u16(top + 8)) == 0x0006u &&
                            (rd_le_u16(top + 2)) == 100u,
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
                    object_set_action_entity(0u);
                    object_action_set_program(1u);
                    object_action_set_cursor(2u);
                    object_action_run();
                    top = object_slot(1);
                    top[0] = 100;
                    top[1] = 0;
                    top[2] = 70;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_set_action_entity(1u);
                    _check(object_stack_push(top) == 1,
                           "the push for the verbs");
                    top[0] = 120; /* off the base's x */
                    _check(object_stack_snap(top) == 1 &&
                               (rd_le_u16(top + 0)) == 100u &&
                               (rd_le_u16(top + 4)) == 0x0020u,
                           "the snap puts the top member on the base's x");
                    /* step off: on `entry.y - 0x18 == actor.y` the actor and the
                     * list byte below it both move down 8 */
                    top[2] = 76;
                    top[3] = 0;
                    _check(object_stack_step_off(top) == 1 &&
                               (rd_le_u16(top + 2)) == 84u &&
                               (rd_le_u16(top + 4)) == 0x0020u &&
                               (rd_le_u16(top + 8)) == 8u,
                           "the step off moves the actor down 8 (frame 0x20)");
                    _check((rd_le_u16(base + 2)) == 108u &&
                               (rd_le_u16(base + 4)) == 0x0021u,
                           "the step off moves the record below it too");
                    /* leave: on `entry.y == actor.y` the actor is a standing
                     * human again and the list is cut at its stored slot */
                    top[2] = 100;
                    top[3] = 0;
                    _check(
                        object_stack_leave(top) == 1 &&
                            (rd_le_u16(top + 4)) == 0x0010u &&
                            (rd_le_u16(top + 8)) == 2u,
                        "the leave restores the actor (frame 0x10 / type 2)");
                    _check(object_stack_scan_dissolve(1u) == 0,
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
                    object_set_action_entity(0u);
                    object_action_set_program(1u);
                    object_action_set_cursor(2u);
                    object_action_run();
                    top = object_slot(1);
                    top[0] = 100;
                    top[1] = 0;
                    top[2] = 70;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_set_action_entity(1u);
                    _check(object_stack_push(top) == 1,
                           "the push before the stand");
                    _check(object_stack_stand(top) == 1 &&
                               (rd_le_u16(top + 0)) == 100u &&
                               (rd_le_u16(top + 2)) ==
                                   64u && /* 71 after the push */
                               (rd_le_u16(top + 4)) == 0x0025u &&
                               (rd_le_u16(top + 8)) == 7u &&
                               (rd_le_u16(top + 0xE)) != 0u,
                           "1000:E99D stands the member (x, y-7, frame 0x25)");
                    _check((rd_le_u16(base + 2)) == 92u &&
                               (rd_le_u16(base + 4)) == 0x0020u,
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
                    object_set_action_entity(0u);
                    object_action_set_program(1u);
                    object_action_set_cursor(2u);
                    object_action_run();
                    m1 = object_slot(1);
                    m1[0] = 100;
                    m1[1] = 0;
                    m1[2] = 70;
                    m1[3] = 0;
                    m1[4] = 0x10;
                    m1[6] = 0;
                    m1[8] = 2;
                    m1[9] = 0;
                    object_set_action_entity(1u);
                    _check(object_stack_push(m1) == 1,
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
                    object_set_action_entity(2u);
                    _check(object_stack_push(m2) == 1,
                           "the second member for the slot walk");
                    /* negative control: the owner's y is not this slot's */
                    m2[2] = 76;
                    m2[3] = 0;
                    _check(
                        object_stack_step_off(m2) == -1 &&
                            (rd_le_u16(m1 + 4)) == 0x0025u,
                        "1000:E896: the second member refuses the owner's y");
                    m2[2] = 60;
                    m2[3] = 0;
                    _check(
                        object_stack_step_off(m2) == 1 &&
                            (rd_le_u16(m2 + 2)) == 68u &&
                            (rd_le_u16(m2 + 4)) == 0x0020u &&
                            (rd_le_u16(m2 + 8)) == 8u,
                        "1000:E896: the second member steps off at entry.y - 0x28");
                    _check(
                        (rd_le_u16(m1 + 2)) == 79u &&
                            (rd_le_u16(m1 + 4)) == 0x0021u,
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
                    object_set_action_entity(0u);
                    object_action_set_program(1u);
                    object_action_set_cursor(2u);
                    object_action_run();
                    top = object_slot(1);
                    top[0] = 100;
                    top[1] = 0;
                    top[2] = 70;
                    top[3] = 0;
                    top[4] = 0x10;
                    top[6] = 0;
                    top[8] = 2;
                    top[9] = 0;
                    object_set_action_entity(1u);
                    _check(object_stack_push(top) == 1,
                           "the push for the selector");
                    /* frame 0x25 is not the stack pose, so a near target hands
                     * over to the walk step (1000:E6FB -> 1000:F25F, the level's
                     * two maps).  It either steps or refuses whole. */
                    {
                        uint16_t x0 = (uint16_t)(rd_le_u16(top + 0));

                        _check(object_selector(top, 1u, 8u, 1u) == 0 &&
                                   (rd_le_u16(top + 4)) == 0x0009u &&
                                   (uint16_t)(rd_le_u16(top + 0)) ==
                                       (uint16_t)(x0 - 1u),
                               "a far target resets the frame, then walks "
                               "(1000:F20C -> F212)");
                    }
                    /* in the stack pose the selector is a plain frame write */
                    _check(object_stack_snap(top) == 1,
                           "the snap before the selector");
                    _check(
                        object_selector(top, 1u, 8u, 1u) == 0 &&
                            (rd_le_u16(top + 4)) == 8u,
                        "the selector writes the target frame in the stack pose");
                    /* a caller x far from the record below removes the member;
                     * the unmarked 0x64AD/0x64C1 bit then runs 1000:E389 and
                     * 1000:F223, which either takes the entry's x or stops on a
                     * special tile (field K's [0x6F20]/[0x6F21]) */
                    top[0] = 140;
                    top[1] = 0;
                    _check(object_selector(top, 200u, 8u, 1u) == 0 &&
                               (rd_le_u16(top + 8)) == 1u,
                           "the selector's removal sets the wrapper's type");
                    /* 1000:E361 aligns the probe row (AND BX,0xFFF0), so the
                     * distance test is 1 px and F1D7 returns before F223's x
                     * write; the old 100 was the unaligned reading. */
                    _check(
                        (rd_le_u16(top + 0)) == 140u,
                        "1000:F1D7 rejects the 1-px step before F223's x write");
                    _check((rd_le_u16(base + 2)) == 92u &&
                               (rd_le_u16(base + 4)) == 0x0020u,
                           "the record below moves up 8 and takes frame 0x20");
                    below = NULL;
                    _check(object_stack_pop(100u, &below, NULL) == -1 &&
                               object_stack_scan_dissolve(1u) == 0,
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
                            object_set_action_entity(0u);
                            object_action_set_program(1u);
                            object_action_set_cursor(2u);
                            object_action_run();
                            top2 = object_slot(1);
                            top2[0] = (uint8_t)x;
                            top2[1] = 0;
                            top2[2] = (uint8_t)(y - 0x1Cu);
                            top2[3] = 0;
                            top2[4] = 0x10;
                            top2[6] = 0;
                            top2[8] = 2;
                            top2[9] = 0;
                            object_set_action_entity(1u);
                            if (object_stack_push(top2) != 1) continue;
                            /* the first call lands the frame in the left cycle
                             * (1000:F20C); the second one is the walk's own
                             * distance test (0 <= frame - 8 < 8) */
                            if (object_selector(top2, 1u, 8u, 1u) != 0)
                                continue;
                            x0 = (uint16_t)(rd_le_u16(top2 + 0));
                            f0 = (uint16_t)(rd_le_u16(top2 + 4));
                            if (object_selector(top2, 1u, 8u, 1u) != 0)
                                continue;
                            x1 = (uint16_t)(rd_le_u16(top2 + 0));
                            if (x1 == x0) {
                                _check(
                                    (rd_le_u16(top2 + 4)) == f0,
                                    "a refused walk step leaves the frame alone");
                                refused++;
                                continue;
                            }
                            _check(x1 == (uint16_t)(x0 - 1u),
                                   "the left walk step moves x by one");
                            _check((uint32_t)(rd_le_u16(top2 + 4)) >= 8u &&
                                       (uint32_t)(rd_le_u16(top2 + 4)) <=
                                           0x000Fu,
                                   "the left walk frame lands in 8..0xF");
                            stepped++;
                            /* the right walk (1000:E764) from the same spot:
                             * [0x4238] = 0 puts the frame in the 0..7 cycle */
                            (void)object_selector(top2, 1u, 0u, 0u);
                            if (object_selector(top2, 1u, 0u, 0u) == 0 &&
                                (uint16_t)(rd_le_u16(top2 + 0)) ==
                                    (uint16_t)(x1 + 1u) &&
                                (uint32_t)(rd_le_u16(top2 + 4)) <= 7u)
                                right++;
                        }
                    }
                    _check(stepped != 0,
                           "some cell of level 1 lets the walk step");
                    _check(refused != 0,
                           "some cell of level 1 refuses the walk step");
                    _check(right != 0, "the right walk step mirrors the left");
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
                    _tf_wr16(w, 0u, 48);
                    _tf_wr16(w, 2u, 24);
                    _tf_wr16(w, 4u, 0x10u);
                    _tf_wr16(w, 8u, 0u);
                    _tf_wr16(w, 0xAu, 0u);
                    _tf_wr16(w, 0xCu, 0u);
                    object_set_action_entity(0u);
                    for (i = 0; i < 40u; i++)
                        (void)object_command_run(w, 0x04u);
                    x = _tf_rd16(w, 0u);
                    _check(x == 21u, "the wall stops the left walk at x 21");

                    object_init();
                    w = object_slot(0);
                    _tf_wr16(w, 0u, 48);
                    _tf_wr16(w, 2u, 24);
                    _tf_wr16(w, 4u, 0x10u);
                    _tf_wr16(w, 8u, 0u);
                    _tf_wr16(w, 0xAu, 0u);
                    _tf_wr16(w, 0xCu, 0u);
                    object_set_action_entity(0u);
                    for (i = 0; i < 41u; i++)
                        (void)object_command_run(w, 0x08u);
                    x = _tf_rd16(w, 0u);
                    y = _tf_rd16(w, 2u);
                    _check(x == 89u && y == 27u,
                           "the right walk settles to y 27 at x 89");
                    for (i = 0; i < 5u; i++) (void)object_command_run(w, 0x08u);
                    x = _tf_rd16(w, 0u);
                    y = _tf_rd16(w, 2u);
                    _check(x == 94u && y == 24u,
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
                    _tf_wr16(h, 0u, 100);
                    _tf_wr16(h, 2u, 120);
                    _tf_wr16(h, 4u, 0x10u);
                    _tf_wr16(h, 8u, 2u);
                    h = object_slot(2);
                    memset(h, 0, 0x12u);
                    _tf_wr16(h, 0u, 200);
                    _tf_wr16(h, 2u, 80);
                    _tf_wr16(h, 4u, 0x10u);
                    _tf_wr16(h, 8u, 2u);
                    object_set_action_entity(1u);
                    m = object_slot(8u);
                    memset(m, 0, 0x12u);
                    _tf_wr16(m, 0u, 0xFFFFu);
                    _tf_wr16(m, 2u, 0xFFFFu);
                    _check(object_cycle_entity(1) == 2,
                           "the selector moves to the next enabled record");
                    _check(
                        _tf_rd16(m, 0u) == 200u &&
                            _tf_rd16(m, 2u) == 80u - 0x1Au &&
                            _tf_rd16(m, 6u) == 0u && _tf_rd16(m, 8u) == 0x1Au &&
                            _tf_rd16(m, 0xAu) == 0x0605u,
                        "1000:4188 arms the marker on the new acting record");
                    object_marker_tick();
                    _check(_tf_rd16(m, 0u) == 200u &&
                               _tf_rd16(m, 2u) == 80u - 0x1Au,
                           "1000:D64F tracks the acting record");
                    for (uint32_t k = 0; k < 6u; k++) object_marker_tick();
                    _check(_tf_rd16(m, 8u) == 0x18u,
                           "the marker's type steps 0x1A -> 0x18 (the bob)");
                    /* the lifetime: type 0x12 and a zero high byte expire */
                    _tf_wr16(m, 8u, 0x12u);
                    _tf_wr16(m, 0xAu, 0x0001u);
                    object_marker_tick(); /* low byte -> 0 */
                    object_marker_tick(); /* wrap: +B 0 -> -1 */
                    _check((m[6] & 0x80u) != 0u,
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
                    _tf_wr16(r0, 8u, 2u);
                    r1 = object_slot(1);
                    memset(r1, 0, 0x12u);
                    _tf_wr16(r1, 8u, 2u);
                    level_set_updates(1);
                    k.keyScan = 0x3Cu; /* F2 */
                    _check(gameflow_key_list(&k) == 1 &&
                               object_action_entity() == 1u,
                           "F2 selects record 1 (1000:38C3)");
                    object_pending_run(); /* the dispatch between key frames */
                    k.keyScan = 0x3Bu; /* F1 */
                    _check(gameflow_key_list(&k) == 1 &&
                               object_action_entity() == 0u,
                           "F1 selects record 0");
                    object_pending_run();
                    k.keyScan = 0x34u; /* '.' next */
                    _check(gameflow_key_list(&k) == 1 &&
                               object_action_entity() == 1u,
                           "'.' steps to the next record (1000:38E4)");
                    object_pending_run();
                    k.keyScan = 0x33u; /* ',' previous */
                    _check(gameflow_key_list(&k) == 1 &&
                               object_action_entity() == 0u,
                           "',' steps to the previous record");
                    object_pending_run();
                    r1[6] = 0x80u; /* disabled */
                    k.keyScan = 0x3Cu;
                    _check(gameflow_key_list(&k) == 1 &&
                               object_action_entity() == 0u,
                           "a disabled record is not selected (1000:38CE)");
                    object_pending_run();
                    r1[6] = 0u;
                    /* SPACE: a convertible pose is stood up, the cell stays */
                    object_action_set_program(1u);
                    cell = statusbar_action_cell(1u);
                    _tf_wr16(r0, 8u, 0x12u);
                    _tf_wr16(r0, 4u, 0x2Bu);
                    k.keyScan = 0x39u;
                    _check(gameflow_key_list(&k) == 1 &&
                               _tf_rd16(r0, 8u) == 0u &&
                               statusbar_action_cell(1u) == cell,
                           "SPACE home-states the acting record (1000:392D)");
                    /* a normal pose steps the row's selected cell instead */
                    k.keyScan = 0x39u;
                    _check(gameflow_key_list(&k) == 1 &&
                               statusbar_action_cell(1u) != cell,
                           "SPACE steps the action row's cell (1000:3933)");
                    /* the [0xC] gate: F2 needs the world open, F10 (id bit 7)
                     * runs regardless */
                    level_set_updates(0);
                    k.keyScan = 0x3Cu;
                    _check(gameflow_key_list(&k) == 0,
                           "the [0xC] gate stops F2 (1000:38AD)");
                    k.keyScan = 0x44u; /* F10 */
                    _check(gameflow_key_list(&k) == 1 &&
                               gameflow_key_wait_active(),
                           "F10 runs with the world gate closed (id bit 7)");
                    /* 1000:3954: release -> press -> release, then back to
                     * the play loop */
                    memset(&k, 0, sizeof k);
                    _check(gameflow_key_list(&k) == 1 &&
                               gameflow_key_wait_active(),
                           "F10 waits for the release");
                    k.lastKey = 'A'; /* a make */
                    _check(gameflow_key_list(&k) == 1 &&
                               gameflow_key_wait_active(),
                           "F10 then waits for the press");
                    memset(&k, 0, sizeof k);
                    _check(gameflow_key_list(&k) == 1 &&
                               !gameflow_key_wait_active(),
                           "F10 releases the world after the second release");
                    level_set_updates(1);
                }

                /* 1000:410A: seven candidates (BX = 6), the records other
                 * than the current one; a lone eligible record is left alone
                 * instead of being re-resolved as the eighth candidate. */
                {
                    uint8_t *r0, *r2;

                    object_init();
                    object_set_action_entity(0u);
                    object_action_set_program(2u);
                    r0 = object_slot(0);
                    memset(r0, 0, 0x12u);
                    r0[8] = 2; /* eligible; the other slots stay disabled */
                    r2 = object_slot(2);
                    memset(r2, 0, 0x12u);
                    r2[6] = 0x80u;
                    _check(object_cycle_entity(1) == -1,
                           "the cycler leaves a lone record alone (1000:4143)");
                    _check(object_action_program() == 2u &&
                               object_action_pending() == 0u,
                           "the cycler does not re-resolve the current record");
                    r2[6] = 0u; /* slot 2 now eligible */
                    _check(object_cycle_entity(1) == 2,
                           "the cycler steps over the disabled slot 1");
                }

                /* 1000:E447/1000:34D1/1000:386E: [0x1979] is a per-call
                 * output state.  A handler that queues code 5 (1000:EEF7)
                 * must not keep re-queuing it, or event 5 (1000:382D)
                 * rewrites the ACTION cursor with [0x1988] on every frame and
                 * a SPACE selection cannot stick.  scan_id_list is skipped
                 * while a code is armed, and a repeat of the last dispatched
                 * code runs no handler. */
                {
                    InputState k;
                    uint8_t *a;

                    object_init();
                    object_set_action_entity(0u);
                    a = object_slot(0);
                    memset(a, 0, 0x12u);
                    _tf_wr16(a, 0u, 0x0080u);
                    _tf_wr16(a, 2u, 0x0050u);
                    _tf_wr16(a, 4u, 0x008Eu); /* a frame in 0x8C..0x93 */
                    _tf_wr16(a, 8u, 0x0012u); /* a walking human type */
                    object_action_set_program(1u);
                    object_action_set_cursor(1u);
                    object_set_action_latched(0);
                    level_set_updates(1);

                    /* the turn queues code 4; the key walk waits for the
                     * dispatch (1000:386E), then the repeat is debounced
                     * (1000:34D1) */
                    (void)object_command_run(a, 0x80u);
                    _check(object_action_pending() == 4u,
                           "the turn queues code 4");
                    memset(&k, 0, sizeof k);
                    k.keyScan = 0x39u; /* SPACE */
                    _check(gameflow_key_list(&k) == 0,
                           "a pending code skips scan_id_list (1000:386E)");
                    object_pending_run();
                    _check(object_action_pending() == 0u,
                           "the dispatch consumes the code");
                    object_set_action_latched(0);
                    (void)object_command_run(a, 0x80u);
                    _check(object_action_pending() == 0u,
                           "a repeated dispatch code is debounced (1000:34D1)");

                    /* the next frame's step enters (1000:EE3B), queues code 5
                     * once; SPACE then moves the cell and no later frame may
                     * reset it (the missing 1000:E447 clear did) */
                    object_set_action_latched(0);
                    (void)object_command_run(a, 0x04u);
                    _check(object_action_pending() == 5u,
                           "the enter sequence queues code 5");
                    object_pending_run();
                    object_space_action();
                    _check(statusbar_action_cell(1u) == 2u,
                           "SPACE selects the second action cell");
                    for (uint32_t f = 0; f < 3u; f++) {
                        object_set_action_latched(0);
                        (void)object_command_run(a, 0x04u);
                        object_pending_run();
                    }
                    _check(statusbar_action_cell(1u) == 2u,
                           "a queued code 5 does not reset the cursor every "
                           "frame (1000:E447)");
                }

                /* The two movement states that feed the stack: 1000:E4F8 (step
                 * down) and 1000:E609 (walk up).  Their feet cell decides a
                 * special tile (field K), a 0x6F92 span, else the grab / the
                 * push.  No 0x6F92 span exists in these tests, so the tile rule
                 * and the dispatch are what is driven. */
                {
                    const uint8_t *map = level_map();
                    uint8_t *a0, *a1;
                    uint32_t sp0 = level_special_tile(0u),
                             sp1 = level_special_tile(1u);
                    int32_t found = 0, col = 0, row = 0;
                    int32_t tileUp = 0, tileDown = 0;

                    _check(map != NULL,
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
                        _check(object_step_up(a0) == 1 &&
                                   (rd_le_u16(a0 + 0)) == 16 * col &&
                                   (rd_le_u16(a0 + 2)) == 16 * row - 0x18 &&
                                   (rd_le_u16(a0 + 4)) == 0x25u &&
                                   (rd_le_u16(a0 + 8)) == 5u,
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
                        _check(
                            object_step_down(a0) == 1 &&
                                (rd_le_u16(a0 + 4)) == 0x23u &&
                                (rd_le_u16(a0 + 8)) == 4u,
                            "the step-down tile rule sets frame 0x23 / type 4");
                        tileDown = 1;
                    }
                    _check(tileUp != 0 && tileDown != 0,
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
                        object_set_action_entity(0u);
                        for (uint32_t t = 0; t < 30u; t++) {
                            uint32_t f;

                            a[2] = (uint8_t)(16 * row - 0xE);
                            a[3] = 0;
                            (void)object_command_run(a, 0x01u);
                            f = (uint32_t)(rd_le_u16(a + 4));
                            if (f < 0x23u || f > 0x25u) outside = 1;
                            seen |= 1u << (f & 0x1Fu);
                        }
                        _check(!outside &&
                                   (seen & (1u << (0x23u & 0x1Fu))) != 0u &&
                                   (seen & (1u << (0x24u & 0x1Fu))) != 0u &&
                                   (seen & (1u << (0x25u & 0x1Fu))) != 0u,
                               "1000:E828: the climb cycles 0x23..0x25");
                        _check(_tf_rd16(a, 2u) ==
                                   (uint32_t)(16 * row - 0xE - 1),
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
                        object_set_action_entity(0u);
                        outside = 0;
                        seen = 0;
                        for (uint32_t t = 0; t < 30u; t++) {
                            uint32_t f;

                            a[2] = (uint8_t)(16 * row - 0x10);
                            a[3] = 0;
                            (void)object_command_run(a, 0x01u);
                            f = (uint32_t)(rd_le_u16(a + 4));
                            if (f < 0x23u || f > 0x25u) outside = 1;
                            seen |= 1u << (f & 0x1Fu);
                        }
                        _check(!outside &&
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
                    object_set_action_entity(0u);
                    _check(object_step_up(a0) == 4,
                           "walk-up with no group does nothing");
                    _check(
                        object_step_down(a0) == 4 &&
                            (rd_le_u16(a0 + 4)) == 0x0020u &&
                            (rd_le_u16(a0 + 8)) == 8u,
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
                            object_set_action_entity(0u);
                            if (object_step_down(a0) != 4) continue;
                            a1 = object_slot(1);
                            a1[0] = 100;
                            a1[1] = 0;
                            a1[2] = (uint8_t)ty;
                            a1[3] = 0;
                            a1[4] = 0x10;
                            a1[6] = 0;
                            a1[8] = 2;
                            a1[9] = 0;
                            object_set_action_entity(1u);
                            if (object_step_up(a1) != 3) continue;
                            _check((rd_le_u16(a1 + 4)) == 0x0025u &&
                                       (rd_le_u16(a1 + 8)) == 7u,
                                   "walk-up pushes (frame 0x25 / type 7)");
                            pushed = 1;
                        }
                        _check(pushed != 0,
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
                        object_set_action_entity(0u);
                        _check(object_step_down(base) == 4,
                               "the base registers");
                        a1 = object_slot(1);
                        a1[0] = 100;
                        a1[1] = 0;
                        a1[2] = 70;
                        a1[3] = 0;
                        a1[4] = 0x10;
                        a1[6] = 0;
                        a1[8] = 2;
                        a1[9] = 0;
                        object_set_action_entity(1u);
                        _check(object_command_run(a1, 0x02u) == OBJ_CMD_HANDLER,
                               "type 2 + bit 1 runs the walk-up handler");
                        _check((rd_le_u16(a1 + 8)) != 2u,
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
                        _check(
                            object_command_run(a1, 0x08u) == OBJ_CMD_HANDLER &&
                                (rd_le_u16(a1 + 4)) == 0u &&
                                (rd_le_u16(a1 + 8)) == 0u &&
                                (rd_le_u16(a1 + 0xA)) == 4u,
                            "command 2 runs the idle handler (frame 0 / type 0)");
                        /* command 6 -> index 4 (1000:E72A): 0x10 -> 8 / type 1 */
                        a1[8] = 2;
                        a1[9] = 0;
                        a1[4] = 0x10;
                        a1[5] = 0;
                        a1[0xA] = 0;
                        a1[0xB] = 0;
                        _check(
                            object_command_run(a1, 0x04u) == OBJ_CMD_HANDLER &&
                                (rd_le_u16(a1 + 4)) == 8u &&
                                (rd_le_u16(a1 + 8)) == 1u,
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
                                if (object_command_run(a1, 0x01u) ==
                                    OBJ_CMD_UNPORTED)
                                    unported++;
                            }
                            _check(unported == 0u,
                                   "every one of the 35 rows runs to its tail");
                        }
                        /* D-16: type 2 command 8 (1000:E7C0) walks the timed
                     * frame stream; the one-shot gate is cleared for the seam. */
                        a1[8] = 2;
                        a1[9] = 0;
                        a1[4] = 0;
                        a1[5] = 0;
                        object_set_command_timer(0u);
                        object_stream_command(a1);
                        _check(
                            (rd_le_u16(a1 + 4)) == 0x10u,
                            "1000:E7C0: type 2 command 8 takes the first stream frame");
                        /* type 15 command 8 (1000:EBCA) clears the +0xA hold */
                        a1[8] = 15;
                        a1[9] = 0;
                        a1[0xA] = 5;
                        a1[0xB] = 0;
                        object_clear_countdown(a1);
                        _check((rd_le_u16(a1 + 0xA)) == 0u,
                               "1000:EBCA: type 15 command 8 clears the hold");
                        _check(object_command_run(a1, 0x00u) !=
                                   OBJ_CMD_UNPORTED,
                               "1000:EBCA is wired into the command table");
                        /* 1000:D6B2 phases 1/2: the pending-move table raises a
                     * record's +4 tag 0x21 -> 0x22 for 0x28 ticks and puts it
                     * back. */
                        {
                            uint8_t *p = object_slot(1);

                            if (p && object_placed_humans() > 1u) {
                                p[4] = 0x21;
                                p[5] = 0;
                                object_animator_reset();
                                object_animator_tick();
                                _check((rd_le_u16(p + 4)) == 0x22u,
                                       "1000:D6B2 phase 1 raises 0x21 -> 0x22");
                                for (uint32_t i = 0; i < 0x29u; i++)
                                    object_animator_tick();
                                _check(
                                    (rd_le_u16(p + 4)) == 0x21u,
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
                            uint8_t *g = object_group_at(7);
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
                                object_animator_reset();
                                for (uint32_t i = 0; i < 7u; i++)
                                    object_animator_tick();
                                _check(
                                    (rd_le_u16(m0 + 0)) == 100u &&
                                        (rd_le_u16(m1 + 0)) == 99u &&
                                        (rd_le_u16(m2 + 0)) == 98u &&
                                        (g[4] & 0x10u) != 0u,
                                    "1000:D6B2: the top member takes -2, the "
                                    "owner 0");
                                /* 1000:D813: the state byte keeps the 0xF0 mask -
                             * the 0x80 the 0xFF clear leaves is the sentinel
                             * the original's backward walk stops on */
                                _check(
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
                        _check(
                            object_command_run(a1, 0x01u) == OBJ_CMD_TAIL,
                            "a row with only a ported tail reports the tail");
                        /* type 15's tail is 1000:DC1F, now ported: the command
                     * runs its handler and reports the tail */
                        a1[8] = 15;
                        a1[9] = 0;
                        _check(
                            object_command_run(a1, 0x01u) == OBJ_CMD_TAIL ||
                                object_command_run(a1, 0x01u) ==
                                    OBJ_CMD_HANDLER,
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
                        n = object_load_terrain_objects(m);
                        _check(n == 10u, "the decorations append two records");
                        r = object_slot(9);
                        _check(r != NULL && (rd_le_u16(r + 0)) == 48u &&
                                   (rd_le_u16(r + 2)) == 32u &&
                                   (rd_le_u16(r + 4)) == 0x00C1u &&
                                   (rd_le_u16(r + 8)) == 0x00C1u,
                               "0xF0 appends frame 0xC1 at (col*16, row*16)");
                        r = object_slot(10);
                        _check(r != NULL && (rd_le_u16(r + 0)) == 112u &&
                                   (rd_le_u16(r + 4)) == 0x00C9u,
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
                        _check(object_load_terrain_objects(m) == 13u,
                               "0xA9 appends its five records");
                        _check(
                            (rd_le_u16(object_slot(9) + 0)) == 48u &&
                                (rd_le_u16(object_slot(9) + 2)) == 0x44u &&
                                (rd_le_u16(object_slot(9) + 4)) == 0x33u,
                            "the first record is (col*16+0x10, row*16+0x14) frame 0x33");
                        _check((rd_le_u16(object_slot(13) + 4)) == 0x37u,
                               "the fifth record is frame 0x37");
                        /* the handler's SI is one past the cell: live level 16 has
                     * 0x33 C2 C9 at cols 14..16 (2026-11-02) */
                        _check(
                            m[4u * 32u + 2u] == 0x00u &&
                                m[4u * 32u + 3u] == 0xC2u &&
                                m[4u * 32u + 4u] == 0xC9u,
                            "0xA9 marks the next two cells a row below as 0xC2/0xC9");
                        /* D-235: the pair has no range test.  Row 18 col 30
                         * puts the first cell at 639 (in the map) and the
                         * second at 640 (the gap); row 19 puts both in the
                         * gap, which the buffer tail holds. */
                        {
                            static uint8_t m9[640];
                            uint8_t *r0;

                            memset(m9, 0, sizeof m9);
                            m9[18u * 32u + 30u] = 0xA9u;
                            object_init();
                            (void)object_load_terrain_objects(m9);
                            _check(m9[639] == 0xC2u,
                                   "D-235: the in-map row-below write at "
                                   "cell 639 lands");
                            memset(m9, 0, sizeof m9);
                            m9[19u * 32u + 0u] = 0xA9u;
                            object_init();
                            (void)object_load_terrain_objects(m9);
                            r0 = object_slot(0);
                            r0[0] = 8u;
                            r0[1] = 0u;
                            r0[2] = 0x34u;
                            r0[3] = 0x01u; /* cell 641 = the gap's first byte */
                            _check(object_record_map_cell(0u) == 0xC2u,
                                   "D-235: a row-19 0xA9 writes the gap "
                                   "(cell 641 = 0xC2)");
                            object_init();
                        }
                        /* 0xB7: two appended records, the only computed class
                     * (+6 = (count-6)<<8) and frame 0x116 / 0x11A */
                        {
                            static uint8_t m3[640], m4[640];

                            memset(m3, 0, sizeof m3);
                            m3[2u * 32u + 2u] = 0xB7u;
                            object_init();
                            _check(object_load_terrain_objects(m3) == 10u,
                                   "0xB7 appends two records");
                            _check(
                                (rd_le_u16(object_slot(9) + 4)) == 0x00D4u &&
                                    (rd_le_u16(object_slot(9) + 6)) ==
                                        0x0200u &&
                                    (rd_le_u16(object_slot(9) + 0xA)) == 0u,
                                "0xB7 with no 0xB6: frame 0xD4, class (count-6)<<8, +0xA = 0");
                            _check(
                                (rd_le_u16(object_slot(10) + 4)) == 0u &&
                                    object_slot_class(1u) == 8u &&
                                    object_slot_class(2u) == 0x85u,
                                "0xB7: behaviour 8 then an empty record with 0x85");
                            /* left neighbour (cell -3): frame 0x116, +0xA += 2 */
                            memset(m4, 0, sizeof m4);
                            m4[2u * 32u + 5u] = 0xB7u;
                            m4[2u * 32u + 2u] = 0xB6u;
                            object_init();
                            (void)object_load_terrain_objects(m4);
                            _check(
                                (rd_le_u16(object_slot(10) + 4)) == 0x0116u &&
                                    (rd_le_u16(object_slot(10) + 0xA)) == 2u,
                                "0xB7's left 0xB6: frame 0x116 and +0xA = 2");
                            /* right neighbour (cell +2): frame 0x11A, +0xA += 3 */
                            memset(m4, 0, sizeof m4);
                            m4[2u * 32u + 5u] = 0xB7u;
                            m4[2u * 32u + 7u] = 0xB6u;
                            object_init();
                            (void)object_load_terrain_objects(m4);
                            _check(
                                (rd_le_u16(object_slot(10) + 4)) == 0x011Au &&
                                    (rd_le_u16(object_slot(10) + 0xA)) == 3u,
                                "0xB7's right 0xB6: frame 0x11A and +0xA = 3");
                            /* both groups: the left test wins */
                            memset(m4, 0, sizeof m4);
                            m4[2u * 32u + 5u] = 0xB7u;
                            m4[2u * 32u + 2u] = 0xB6u;
                            m4[2u * 32u + 7u] = 0xB6u;
                            object_init();
                            (void)object_load_terrain_objects(m4);
                            _check((rd_le_u16(object_slot(11) + 4)) ==
                                           0x0116u &&
                                       (rd_le_u16(object_slot(11) + 0xA)) == 2u,
                                   "0xB7's left 0xB6 wins over the right one");
                            /* 0xBB / 0xC1: two more appends (0x26 / 0x100) */
                            memset(m4, 0, sizeof m4);
                            m4[1u * 32u + 1u] = 0xBBu;
                            m4[1u * 32u + 2u] = 0xC1u;
                            object_init();
                            _check(
                                object_load_terrain_objects(m4) == 10u &&
                                    (rd_le_u16(object_slot(9) + 4)) ==
                                        0x0026u &&
                                    (rd_le_u16(object_slot(10) + 4)) ==
                                        0x0100u &&
                                    (rd_le_u16(object_slot(10) + 2)) == 0u,
                                "0xBB appends 0x26, 0xC1 appends 0x100 at y-0x10");
                            /* 0xBE + 0xBF: the join writes the 0xBF tile's x into the
                         * record 0xBE made (+0xC) */
                            memset(m4, 0, sizeof m4);
                            m4[1u * 32u + 1u] = 0xBEu;
                            m4[1u * 32u + 4u] = 0xBFu;
                            object_init();
                            _check(
                                object_load_terrain_objects(m4) == 9u &&
                                    (rd_le_u16(object_slot(9) + 4)) ==
                                        0x0082u &&
                                    (rd_le_u16(object_slot(9) + 0xC)) == 64u,
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
                            _check(
                                object_load_terrain_objects(m2) == 11u,
                                "the three style tiles append one record each");
                            _check(
                                (rd_le_u16(object_slot(9) + 4)) == 0x96u &&
                                    (rd_le_u16(object_slot(10) + 4)) == 0x9Cu &&
                                    (rd_le_u16(object_slot(11) + 4)) == 0x38u,
                                "campfire 0x96, torch 0x9C, stone 0x38");
                            /* level 1 is style 1, so 1000:5662's +3 nudge must not
                         * apply even with a marked neighbour below-left */
                            _check((rd_le_u16(object_slot(9) + 2)) !=
                                           (uint16_t)(16u + 3u) ||
                                       level_style() == 6u,
                                   "the +3 nudge is gated on the style");
                            _check((rd_le_u16(object_slot(9) + 8)) == 0xFFFFu &&
                                       (rd_le_u16(object_slot(11) + 8)) == 0u &&
                                       (rd_le_u16(object_slot(11) + 0xC)) ==
                                           0xFFFFu,
                                   "the style fields (+8 / +0xC) are per tile");
                        }
                    }
                    /* The +0xA hook, 1000:D8BD (the terrain settle), on a special
                 * tile: the settle returns before writing anything, so the only
                 * change is the idle handler's, and the tail still runs. */
                    {
                        const uint8_t *map = level_map();
                        uint32_t sp0 = level_special_tile(0u),
                                 sp1 = level_special_tile(1u);
                        uint8_t *a1;
                        int32_t found = 0, col = 0, row = 0;

                        for (uint32_t r = 0; r < 640u && !found; r++)
                            if (map[r] == sp0 || map[r] == sp1) {
                                found = 1;
                                row = (int32_t)(r / 32u);
                                col = (int32_t)(r % 32u);
                            }
                        _check(found != 0,
                               "a special tile for the settle test");
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
                        object_set_action_entity(0u);
                        /* command 6 (input 0x04) -> 1000:E72A leaves the type at 2
                     * (frame 0x10), so the hook runs; the feet are on a special
                     * tile, which stops the settle before it writes anything */
                        _check(
                            object_command_run(a1, 0x04u) == OBJ_CMD_TAIL,
                            "the settle hook runs and the tail still follows");
                        _check(
                            (rd_le_u16(a1 + 2)) ==
                                    (uint16_t)(16 * row - 0x18) &&
                                (rd_le_u16(a1 + 4)) == 0x0010u &&
                                (rd_le_u16(a1 + 8)) == 2u,
                            "a special tile stops the settle before it writes");
                    }
                }
                /* the four placed humans again for the second-record _check */
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
                _check(wrong == 0u, "the second placed record is drawn too");
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
                object_set_slot_class(5u, 5u); /* behaviour class 5 */
                memset(carried, 0, 0x12u);
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
                _check((rd_le_u16(carried + 0)) == 52u &&
                           (rd_le_u16(carried + 2)) == 33u &&
                           (rd_le_u16(carried + 4)) == 0x5Au,
                       "the class overlay moves and frames the carried record");
                rec[7] = 0u;
                object_set_slot_class(5u, 0u);
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
                _check(object_load_terrain_objects(m5) == 9u,
                       "the 0xAB spear for the taken-record walk");
                sp = object_slot(9);
                sp[0] = 10;
                sp[1] = 0;
                sp[2] = 10;
                sp[3] = 0;
                _check((rd_le_u16(sp + 4)) == 0x0060u &&
                           object_slot_class(1u) == 5u,
                       "the spear sits in live slot 1");
                object_set_slot_class(1u, 0x85u); /* the pickup's mark */
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
                ink = 0;
                for (uint32_t y = 10; y < 26; y++)
                    for (uint32_t x = 10; x < 26; x++)
                        if (page[y * 320u + x]) ink++;
                _check(ink == 0u,
                       "1000:6450: a taken record is not drawn by the walk");
                /* control: with the mark clear the same record draws */
                object_set_slot_class(1u, 5u);
                memset(page, 0, sizeof page);
                object_render(page, 0, 0);
                ink = 0;
                for (uint32_t y = 10; y < 26; y++)
                    for (uint32_t x = 10; x < 26; x++)
                        if (page[y * 320u + x]) ink++;
                _check(ink != 0u,
                       "the same record draws while it is not taken");
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
                (void)object_load_terrain_objects(m6);
                spear = object_slot(9u); /* live ordinal 1 */
                cand = object_slot(10u); /* live ordinal 2 */
                _check(spear && cand && object_slot_class(1u) == 5u &&
                           object_slot_class(2u) == 8u,
                       "the mover and the behaviour-8 candidate");
                /* the synthetic map is not the loaded level's map, so the
                 * 1000:5639 ground snap probes the level's terrain: level 1
                 * blocks (72,80) and the original parks the record at
                 * 0xFFEF (D-124).  Pin both where this fixture wants them. */
                spear[0] = 64;
                spear[1] = 0;
                spear[2] = 64;
                spear[3] = 0;
                /* frame 0x60 selects (dx,dy) = (+8,+15); the window's far
                 * edges are x - 0x10 and y - 0x1A */
                cand[0] = (uint8_t)(64 + 8 - 0x10);
                cand[1] = 0u;
                cand[2] = (uint8_t)(64 + 15 - 0x1A);
                cand[3] = 0u;
                cand[6] &= 0x7Fu;
                cand[7] = 0u;
                lifeBefore = (uint16_t)(rd_le_u16(spear + 0xE));
                object_update_list();
                _check(cand[7] == 1u,
                       "the candidate links to the acting ordinal");
                _check(object_slot_class(1u) == 0x85u,
                       "the acting record's class byte takes 0x80");
                _check((rd_le_u16(spear + 6)) == 0u,
                       "the acting record's +6 word is cleared");
                _check((uint16_t)(rd_le_u16(spear + 0xE)) == lifeBefore,
                       "the acting record's +0xE is not cleared");
            }
            /* D-229: the hit branch of 0x5A2F clears the MOVER's behaviour
             * byte (1000:5B13..0x5B24), not the victim's.  The behaviour-8
             * enemy must keep its script so state 6's exit runs 0x28..0x2E and
             * frees it; the destroyed spear must stop being dispatched. */
            {
                static uint8_t m7[640];
                uint8_t *sp, *en;
                uint32_t t;

                memset(m7, 0, sizeof m7);
                m7[4u * 32u + 4u] = 0xABu; /* the mover, at (64,64) */
                m7[6u * 32u + 6u] = 0xB7u; /* the behaviour-8 enemy caveman */
                object_init();
                (void)object_load_terrain_objects(m7);
                sp = object_slot(9u);
                en = object_slot(10u);
                /* the 0x6612 stepper runs on odd frames only: keep the
                 * collision projection fixed for the fixture */
                while ((object_frame_counter_probe() & 1u) != 0u)
                    object_tick(NULL);
                sp[0] = 64;
                sp[1] = 0;
                sp[2] = 64;
                sp[3] = 0;
                sp[6] = 0u;
                sp[7] = 0u;
                wr_le_u16(sp + 0xCu, 0u);
                wr_le_u16(sp + 0xEu, 2u); /* the `+0xE > 1` hit gate */
                en[0] = (uint8_t)(64 + 8 - 0x10);
                en[1] = 0u;
                en[2] = (uint8_t)(64 + 15 - 0x1A);
                en[3] = 0u;
                en[6] &= 0x7Fu;
                en[7] = 0u;
                _check(object_slot_class(1u) == 5u &&
                           object_slot_class(2u) == 8u,
                       "D-229: the mover is behaviour 5 and the enemy 8");
                object_update_list();
                _check(
                    rd_le_u16(en + 4u) == 0x0028u && rd_le_u16(en + 8u) == 2u &&
                        rd_le_u16(en + 0xAu) == 6u,
                    "D-229: the hit+same-pass state-6 tick leaves frame 0x28 / +8 2 / +0xA 6");
                _check(object_slot_class(2u) == 8u,
                       "D-229: the victim keeps its behaviour 8");
                _check(
                    object_slot_class(1u) == 0u && sp[6] == 0xFFu,
                    "D-229: the destroyed spear's behaviour and +6 are cleared");
                for (t = 0; t < 40u && en[6] != 0xFFu; t++)
                    object_update_list();
                _check(
                    rd_le_u16(en + 4u) == 0x002Eu && en[6] == 0xFFu &&
                        object_slot_class(2u) == 0u,
                    "D-229: behaviour 8's state-6 exit runs to 0x2E and frees the enemy");
            }
            object_publish_bank(NULL, 0u);
            free(bank);
        }
        object_init();
    } else printf("skip: the level data is not on the disc\n");

    /* 7. draw_status_indicator (1000:1FEF): a present volume returns at
     * once (no screen); a missing one draws the prompt and waits */
    _check(res_volume_present(1u), "disc 1 is present");
    _check(level_disk_status_begin(1u) == 0, "a present disc skips the prompt");
    {
        _check(!res_volume_present(3u), "disc 3 is the missing-volume case");
        _check(level_disk_status_begin(3u) == 1,
               "a missing disc draws the prompt");
        {
            InputState k;
            memset(&k, 0, sizeof k);
            /* 1000:2042's timing_loop runs before the page: the first tick
             * draws it and starts the palette fade. */
            _check(level_disk_status_tick(&k) == 1, "the prompt starts");
            _check(palette_fade_active(), "the prompt fades its palette in");
            for (uint32_t t = 0; t < 80u && palette_fade_active(); t++)
                palette_tick();
            _check(!palette_fade_active(), "the prompt fade completes");
            k.lastKey = 1;
            _check(level_disk_status_tick(&k) == 1,
                   "the prompt waits while the disc is still missing");
        }
        level_disk_status_reset();
        _check(res_volume_present(0u), "disc 0 is present");
    }

    /* 7b. the post-level cutscene decision (entry 14E6/14FF) and the
     * ending index (150C/1512), against the real header bytes */
    _check(gameflow_post_level_scene(1, 0u) == 0u,
           "a won level with intro 0 plays scene 0");
    _check(gameflow_post_level_scene(1, 0xFFu) == 0xFFFFu,
           "intro 0xFF = no newspaper");
    _check(gameflow_post_level_scene(0, 0u) == 0xFFFFu,
           "a lost level never plays its newspaper");
    _check(gameflow_ending_scene(1) == 7u && gameflow_ending_scene(0) == 6u,
           "ending scene 7 COMPLETE / 6 lost");
    if (level_load(0) == 0)
        _check(level_data()->introIndex == 0u, "level 1's header intro is 0");
    if (level_load(1) == 0)
        _check(level_data()->introIndex == 0xFFu,
               "level 2's header intro is 0xFF (no newspaper)");
    if (level_load(5) == 0)
        _check(level_data()->introIndex == 4u, "level 6's header intro is 4");

    /* 7b2. D-55: the briefing's "THE CODE FOR THIS LEVEL IS....." value is
     * the level record's author (load_level 1000:30BC copies it to 0x1189),
     * not a fixed string. */
    if (level_load(1) == 0)
        _check(strcmp(gameflow_party_tribe(), "ANDIE PANDY") == 0,
               "level 2's briefing code is its author");
    if (level_load(3) == 0)
        _check(strcmp(gameflow_party_tribe(), "CARLOS") == 0,
               "level 4's briefing code follows the level");

    /* 6c2b2. D-112 (closed 2026-11-05): 1000:5662's style-6 +3 nudge, which
     * the 0xB9 exit-marker stamper (0x54A9) runs before ADD DX,0x10 /
     * SUB DX,0x40.  Level 28's 0xB9 at (8, 4) has cell 112 below (the 0x1F20
     * bit is set); level 29's 0xB9 at (13, 3) has cell 15 (not set). */
    if (level_load(28) == 0) {
        _check(level_style() == 6u, "D-112: level 28 is style 6");
        _check(object_style_y_nudge(8u, 4u) == 3u,
               "D-112: the 0xB9 cell's nudge is 3 rows");
        _check(level_exit_stamp_y(8u, 4u) == 4 * 16 + 3 - 0x30,
               "D-112: the exit stamp is 3 rows higher");
    }
    if (level_load(29) == 0)
        _check(object_style_y_nudge(13u, 3u) == 0u &&
                   level_exit_stamp_y(13u, 3u) == 3 * 16 - 0x30,
               "D-112: level 29's 0xB9 cell gets no nudge");

    /* 6c2c. D-109 (closed 2026-11-05): 1000:C541's mode-0 re-show clears
     * [r+6] bit 7 and returns to 1000:C447, which re-tests the now-clear flag
     * and runs the (x-3, y-8) pickup scan; the port used to return from every
     * 0xC541 arm and skip that frame's scan.  The same re-test covers the
     * 0x26 arm too (it leaves the flag as it was). */
    object_init();
    if (level_load(0) == 0) {
        uint8_t *m = object_slot(9u), *s0 = object_slot(0u);
        const uint8_t *map = level_map();
        uint8_t cell0 = map ? map[0] : 0u;

        _check(m != NULL && s0 != NULL, "D-109: the spear is record 9");
        if (m && s0) {
            object_set_marker_mode(0);
            object_set_slot_class(1u, 9u); /* record 9 -> behaviour 9 */
            /* the static record on the marker's tile-aligned cell, type 0 */
            memset(s0, 0, 0x12u);
            _tf_wr16(s0, 0u, 96u);
            _tf_wr16(s0, 2u, 92u);
            _tf_wr16(s0, 4u, 0x10u);
            _tf_wr16(s0, 8u, 0x0000u);
            /* the marker: re-show bit set, frame != 0x26, map cell 0 */
            memset(m, 0, 0x12u);
            _tf_wr16(m, 0u, 100u);
            _tf_wr16(m, 2u, 100u);
            _tf_wr16(m, 4u, 0x10u);
            m[6] = 0x80u;
            _tf_wr16(m, 8u, 0x0000u);
            while ((object_frame_counter_probe() & 3u) != 0u) object_tick(NULL);
            object_update_list();
            _check((m[6] & 0x80u) == 0u,
                   "D-109: the re-show clears [+6] bit 7");
            _check(_tf_rd16(s0, 4u) == 0x28u && _tf_rd16(s0, 8u) == 0x16u &&
                       _tf_rd16(s0, 0xAu) == 5u,
                   "D-109: and runs the pickup scan on the human slot");
            _check(_tf_rd16(m, 4u) == 0x10u && _tf_rd16(m, 8u) == 0u,
                   "D-228: the marker keeps its frame and map cell");
        }
        if (map) ((uint8_t *)map)[0] = cell0;
    }

    /* 6c3. D-64/D-65: a successful pickup sets [0x5B56] = [0x5B8A + class]
     * (class 5 -> 2, the bar's 4-icon spear group), and op 6 with the class
     * set runs the class path 1000:9D5D: the carried record is thrown at the
     * acting record's feet and the bar returns to group 1. */
    object_init(); /* fresh array: 6c2 marked the spear */
    if (level_load(0) == 0) {
        uint8_t *actor = object_slot(0), *spear = object_slot(9);

        /* a level start leaves [0x1988] = 1, so prog[1] = 1 and the press
         * reads prog[2] = op 2; earlier blocks may have left a stale cursor */
        object_action_set_cursor(1u);
        _check(actor != NULL && spear != NULL &&
                   (rd_le_u16(spear + 4)) == 0x0060u,
               "the spear is live at index 9");
        object_set_action_entity(0u);
        actor[0] = (uint8_t)(448u - 8u);
        actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u;
        actor[3] = 0;
        actor[4] = 0x10;
        actor[6] = 0;
        actor[8] = 2;
        actor[9] = 0;
        _check(object_action_program() == 1u && statusbar_action_group() == 1u,
               "the bar starts on group 1");
        object_action_run();
        _check(actor[7] == 1u && object_slot_class(1u) == 0x85u,
               "the pickup takes the spear");
        _check(object_action_program() == 2u && statusbar_action_group() == 2u,
               "the pickup moves the bar to the spear's group 2");
        _check(statusbar_action_count(1u) == 2u &&
                   statusbar_action_count(2u) == 4u,
               "group 2 shows four icons where group 1 shows two");
        /* program 2's opcode walk is prog[2..5] = 3,4,5,6 - cursor 4 is op 6 */
        object_action_set_cursor(4u);
        object_action_run();
        _check(object_action_program() == 1u && statusbar_action_group() == 1u,
               "dropping it puts the bar back on group 1");
        _check(actor[7] == 0u && object_slot_class(1u) == 0x05u,
               "the class and the taken mark are cleared");
        /* 1000:E361 aligns the probe row: row 48 + the 0xAB page byte's bit 7
         * leaves BX = 48, so y = 48 - 0xF = 33 (the old 41 was unaligned). */
        _check((rd_le_u16(spear + 0)) == 440u &&
                   (rd_le_u16(spear + 2)) == 33u &&
                   (rd_le_u16(spear + 4)) == 0x0060u &&
                   (rd_le_u16(spear + 8)) == 0xFFFFu,
               "the spear is thrown to the actor's feet, frame 0x60");
        object_action_set_cursor(1u);
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

        for (i = 1; i <= object_live_count() - 8u; i++)
            if (object_slot_class(i) == 5u) {
                ord = i;
                break;
            }
        _check(ord != 0u, "level 2 has a spear for the release test");
        if (ord) {
            it = object_slot(8u + ord);
            object_set_action_entity(0u);
            object_action_set_cursor(1u);
            object_set_action_latched(0);
            actor[0] = (uint8_t)((rd_le_u16(it + 0)) - 8u);
            actor[1] = (uint8_t)(((rd_le_u16(it + 0)) - 8u) >> 8);
            actor[2] = it[2];
            actor[3] = it[3];
            actor[4] = 0x10;
            actor[6] = 0;
            actor[8] = 2;
            actor[9] = 0;
            object_action_run(); /* the pickup */
            object_action_set_cursor(2u);
            actor[8] = 1; /* op 4's gate is CMP AX,2 / JNC (D-113) */
            actor[9] = 0;
            actor[4] = 0x10;
            object_action_run(); /* op 4: the pose */
            _check((uint32_t)(rd_le_u16(actor + 4)) == 0x00BEu,
                   "the throw pose is up before the release");
            /* the press frame: the fire half selects the turn; pause_toggle
             * is gated on [0x6F1D], so a posed record keeps [0x13A4] clear
             * (live trace: act 00, joy 80 while Z is held) */
            object_set_action_latched(0);
            object_command_run(actor, 0x80u);
            _check((uint32_t)(rd_le_u16(actor + 0xEu)) == 4u,
                   "the fire press starts the aim (+0xE = 4)");
            /* the next frame: the fire is masked, the idle form releases */
            object_command_run(actor, 0x00u);
            /* 0xEFE4 bumps type +2 / frame +1 and then runs the fall machine,
             * so the immediate result is the landing state (type 0/1), never
             * the pose again. */
            _check(!((uint32_t)(rd_le_u16(actor + 4)) == 0x00BEu &&
                     (uint32_t)(rd_le_u16(actor + 8)) == 0x0018u),
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

        object_set_action_entity(0u);
        actor[6] = 0;
        actor[7] = 0;
        actor[8] = 0x18;
        actor[9] = 0; /* the throw pose's type */
        actor[4] = 0xBE;
        actor[5] = 0;
        c = object_command_select(actor, 0x00u);
        _check(c.handler == 0xEFD7u,
               "the no-input command selects the pose row's row[9] (0xEFD7)");
        c = object_command_select(actor, 0x80u);
        _check(
            c.handler == 0xEFD4u,
            "the fire-only command selects the fire half's row[0x19] (0xEFD4)");
        /* 0xEFD1 is `JMP 0x1000:EDB7` (the home state), selected by the pose
         * row's position 5 with a direction held (row[5] = 0x2F). */
        c = object_command_select(actor, 0x02u);
        _check(c.handler == 0xEFD1u,
               "the DOWN command selects the pose row's row[5] (0xEFD1)");
        _tf_wr16(actor, 4u, 0x00BEu);
        _tf_wr16(actor, 8u, 0x0018u);
        object_set_action_latched(0);
        (void)object_command_run(actor, 0x02u);
        _check(_tf_rd16(actor, 4u) == 8u && _tf_rd16(actor, 8u) == 1u,
               "0xEFD1 home-states the record (1000:EDB7)");
    }

    /* 6c9. ACTION op 8 (1000:EA71) and op 2/6's tail (1000:9CD9).  Op 8's
     * span reset clears unless BOTH +6 and +8 are zero (the `&&` used to miss
     * `+8 != 0`); op 2/6 with no live records takes the tail with the shared
     * [0x423C] programme scratch instead of returning. */
    object_init();
    {
        uint8_t *a = object_slot(0);
        uint8_t *sp = object_span_at_for_probe(0u);
        uint8_t map[640];
        uint32_t parity = object_frame_counter_probe() & 1u;

        object_set_action_entity(0u);
        object_action_set_program(3u); /* 3 = {4,1,3,7,8,6}: cell 3 = op 8 */
        object_action_set_cursor(3u);
        while ((object_frame_counter_probe() & 1u) == 0u) object_tick(NULL);

        /* (a) +8 != 0 and +6 <= +8: the original clears (1000:EAA3) */
        memset(sp, 0, 0x1Cu);
        wr_le_u16(sp + 0u, 0u);
        wr_le_u16(sp + 6u, 4u);
        wr_le_u16(sp + 8u, 8u);
        wr_le_u16(sp + 0x12u, 1u);
        object_set_action_latched(0);
        object_action_run();
        _check(rd_le_u16(sp + 8u) == 0u && rd_le_u16(sp + 0x12u) == 0u,
               "op 8 clears a span with +8 set (1000:EAA3)");

        /* (b) +8 == 0, +6 > 0: the original clears too (1000:EAA1) */
        wr_le_u16(sp + 6u, 4u);
        wr_le_u16(sp + 8u, 0u);
        wr_le_u16(sp + 0x12u, 1u);
        object_set_action_latched(0);
        object_action_run();
        _check(rd_le_u16(sp + 8u) == 0u && rd_le_u16(sp + 0x12u) == 0u,
               "op 8 clears a span with +6 only (1000:EAA1)");

        /* (c) both zero: left alone (1000:EAAB) */
        wr_le_u16(sp + 6u, 0u);
        wr_le_u16(sp + 8u, 0u);
        wr_le_u16(sp + 0x12u, 0x1234u);
        object_set_action_latched(0);
        object_action_run();
        _check(rd_le_u16(sp + 0x12u) == 0x1234u,
               "op 8 leaves a zero span alone (1000:EAAB)");
        /* keep the frame parity the later blocks rely on */
        while ((object_frame_counter_probe() & 1u) != parity) object_tick(NULL);

        /* a pickup stamps [0x423C] = 2 (1000:9C8D) */
        object_init();
        memset(map, 0, sizeof map);
        map[100] = 0xAB; /* a spear at x = 64, y = 48 */
        object_load_terrain_objects(map);
        /* the synthetic map is not the loaded level's map, so the 1000:5639
         * ground snap probes the level's terrain (D-124); pin the spear where
         * this fixture wants it. */
        {
            uint8_t *sp = object_slot(9);

            sp[0] = 64;
            sp[1] = 0;
            sp[2] = 48;
            sp[3] = 0;
        }
        a = object_slot(0);
        memset(a, 0, 0x12u);
        wr_le_u16(a + 0u, 56u);
        wr_le_u16(a + 2u, 48u);
        wr_le_u16(a + 8u, 0u);
        object_set_action_entity(0u);
        object_action_set_program(1u);
        object_action_set_cursor(1u);
        object_set_action_latched(0);
        object_action_run(); /* op 2: the pickup */
        _check(object_action_program() == 2u && object_action_pending() == 5u,
               "the pickup stamps and queues programme 2");

        /* [0x423C] survives a level reset (1000:4CAB); the empty-map press
         * runs the tail with it (1000:9C4E -> 0x9CD9) */
        object_init();
        memset(map, 0, sizeof map);
        object_load_terrain_objects(map); /* live count 8: no live records */
        a = object_slot(0);
        memset(a, 0, 0x12u);
        wr_le_u16(a + 8u, 0u);
        object_set_action_entity(0u);
        object_action_set_program(1u);
        object_action_set_cursor(1u);
        object_set_action_latched(0);
        object_action_run(); /* op 2 on an empty map */
        _check(object_action_pending() == 5u && object_action_program() == 2u,
               "the empty-map tail queues event 5 with the stale programme");

        /* a stale id with bit 7 set skips the tail (1000:9CDF) */
        object_init();
        object_load_terrain_objects(map);
        a = object_slot(0);
        memset(a, 0, 0x12u);
        object_set_action_entity(0u);
        object_action_set_program(1u);
        object_action_set_cursor(1u);
        (void)object_selector(a, 0u, 0u, 0x86u); /* [0x423C] = 0x86 */
        wr_le_u16(a + 8u, 0u);
        object_set_action_latched(0);
        object_action_run();
        _check(object_action_pending() == 0u,
               "a bit-7 stale programme id skips the tail (1000:9CDF)");

        /* the raw id is stored; ids 1..6 resolve their table runs, anything
         * else has no header (D-186) */
        object_init();
        object_action_set_program(0u);
        _check(object_action_program_si() == 0u, "id 0 has no header (D-186)");
        object_action_set_program(1u);
        _check(object_action_program_si() == 0x5B37u,
               "id 1 resolves to the table's first run");

        /* an out-of-table stale id (0x10, what a walk leaves in [0x423C]) is
         * stored raw (D-185) but has no header: no DS read, no draw and no op
         * (D-186: the DS image is gone, out-of-table ids are bounded). */
        object_init();
        object_load_terrain_objects(map);
        a = object_slot(0);
        memset(a, 0, 0x12u);
        object_set_action_entity(0u);
        object_action_set_program(1u);
        object_action_set_cursor(1u);
        (void)object_selector(a, 0u, 0u, 0x10u); /* [0x423C] = 0x10 */
        wr_le_u16(a + 8u, 0u);
        object_set_action_latched(0);
        object_action_run(); /* op 2 -> the tail */
        _check(object_action_program() == 0x10u &&
                   object_action_pending() == 5u,
               "an out-of-table stale id is stored raw (1000:9CEB)");
        _check(object_action_program_si() == 0u &&
                   object_action_byte(0u) == 0u && object_action_byte(1u) == 0u,
               "an out-of-table id has no header (D-186)");
        _check(statusbar_action_count(0x10u) == 0u &&
                   statusbar_action_cell(0x10u) == 0u,
               "the bar draws no out-of-table header");
        object_pending_run(); /* event 5: no header to rewrite */
        object_space_action(); /* SPACE: no cursor to step */
        object_action_run(); /* ACTION: no op to dispatch */
        _check(object_action_program() == 0x10u && !object_action_pending(),
               "an out-of-table id survives the press paths");

        /* event 5's store needs a header: an id outside 1..6 writes nothing
         * (D-186) */
        object_init();
        object_load_terrain_objects(map);
        a = object_slot(0);
        memset(a, 0, 0x12u);
        object_set_action_entity(0u);
        object_action_set_program(2u);
        object_action_set_cursor(1u); /* pin group 2's cursor (0x5B3C) */
        object_action_set_program(1u);
        object_action_set_cursor(1u);
        (void)object_selector(a, 0u, 0u, 9u); /* [0x423C] = 9 */
        wr_le_u16(a + 8u, 0u);
        object_set_action_latched(0);
        object_action_run();
        _check(object_action_program() == 9u &&
                   object_action_program_si() == 0u &&
                   object_action_byte(1u) == 0u,
               "id 9 has no header (D-186)");
        object_pending_run(); /* 1000:382D has no [SI+1] to write */
        _check(object_action_byte(1u) == 0u,
               "event 5 leaves an out-of-table id alone");

        /* an out-of-table header dispatches nothing (D-186) */
        object_init();
        object_load_terrain_objects(map);
        a = object_slot(0);
        memset(a, 0, 0x12u);
        wr_le_u16(a + 4u, 0x10u);
        wr_le_u16(a + 8u, 0u);
        object_set_action_entity(0u);
        object_action_set_program(0x3Fu);
        object_set_action_latched(0);
        object_action_run();
        _check(rd_le_u16(a + 4u) == 0x10u && rd_le_u16(a + 8u) == 0u,
               "an out-of-table header dispatches no op (D-186)");
        object_action_set_program(1u);

        /* the save's +0x06 byte is the raw [0x5B56] (re_formats.md) */
        {
            uint8_t pb[0x20];

            object_action_set_program(0x10u);
            object_save_programs(pb);
            _check(pb[0x1F] == 0x10u, "the save stores the raw program byte");
            object_action_set_program(2u);
            object_load_programs(pb);
            _check(object_action_program() == 0x10u,
                   "the load restores the raw program byte");
            object_action_set_program(1u);
        }
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

        for (i = 1; i <= object_live_count() - 8u; i++)
            if (object_slot_class(i) == 5u) {
                ord = i;
                break;
            }
        _check(ord != 0u, "level 2 has a spear");
        if (ord) {
            it = object_slot(8u + ord);
            object_set_action_entity(0u);
            object_action_set_cursor(1u);
            actor[0] = (uint8_t)((rd_le_u16(it + 0)) - 8u);
            actor[1] = (uint8_t)(((rd_le_u16(it + 0)) - 8u) >> 8);
            actor[2] = it[2];
            actor[3] = it[3];
            actor[4] = 0x10;
            actor[6] = 0;
            actor[8] = 2;
            actor[9] = 0;
            object_action_run(); /* pickup */
            object_action_set_cursor(2u);
            actor[8] = 1; /* op 4's gate is CMP AX,2 / JNC (D-113) */
            actor[9] = 0;
            actor[4] = 0x10;
            object_action_run(); /* op 4: the pose */
            _check((uint32_t)(rd_le_u16(actor + 4)) == 0x00BEu &&
                       (uint32_t)(rd_le_u16(actor + 8)) == 0x0018u,
                   "the throw pose is up (frame 0xBE, type 0x18)");
            /* earlier blocks may have left the ACTION latch or the [0x6F1D]
             * sequence latch set: SPACE home-states the pose (clearing both)
             * and the op re-poses, so the turn starts from a known state */
            object_set_action_latched(0);
            object_space_action();
            object_set_action_latched(
                0); /* 0xEE01 sets it; a release clears it */
            object_action_set_cursor(2u);
            actor[8] = 1;
            actor[9] = 0;
            actor[4] = 0x10;
            object_action_run();
            for (i = 0; i < 60u; i++) statusbar_tick(); /* split -> 152 */
            split = statusbar_split();
            object_set_action_latched(0); /* poll_input 0x85A9 */
            object_command_run(actor, 0x80u); /* fire: the turn (0xEFD4) */
            _check((uint32_t)(rd_le_u16(actor + 0xEu)) == 4u,
                   "the turn starts the charge (+0xE = 4)");
            object_pending_run(); /* event 4: enter the loop */
            _check(object_charge_tick() == 1, "the charge loop runs");
            {
                static const uint8_t shade[10] = {
                    0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE3, 0xE2, 0xE1, 0xE0, 0xDF
                };
                uint32_t v = 4u, ax = 0x60u + (v >> 1u);
                uint32_t x = 4u * (ax >> 2) + (ax & 3u), _bad = 0, r;

                for (r = 0; r < 10u; r++)
                    if (video_page(0)[(split + 15u + r) * 320u + x] != shade[r])
                        _bad++;
                _check(_bad == 0u, "the charge bar is drawn at +0xE = 4");
            }
            object_command_run(actor, 0x80u); /* another turn (fire) frame */
            object_pending_run();
            _check(object_charge_tick() == 1, "the loop keeps running");
            {
                uint32_t v = 8u, ax = 0x60u + (v >> 1u);
                uint32_t x = 4u * (ax >> 2) + (ax & 3u);

                _check(video_page(0)[(split + 15u) * 320u + x] == 0xDFu,
                       "the bar follows +0xE");
            }
            /* the release (no fire) runs the type-0x18 idle half: 0xEFD7 ->
             * 0xEFE4 performs the jump/throw and arms no code. */
            {
                uint32_t f0 = rd_le_u16(actor + 4);
                uint32_t t0 = rd_le_u16(actor + 8);

                object_set_action_latched(0);
                object_command_run(actor, 0x00u);
                _check((uint32_t)(rd_le_u16(actor + 4)) != f0 ||
                           (uint32_t)(rd_le_u16(actor + 8)) != t0,
                       "the release performs the jump/throw (0xEFE4)");
            }
            _check(object_charge_tick() == 0,
                   "the release ends the loop (0x370C)");

            /* D-190: 0x373B is a *word* store at 0x1979, so the charge loop's
             * exit clears the debounce byte 0x197A as well; the next wind-up
             * charge must arm event 4 again (and draw the bar). */
            object_set_action_latched(0);
            actor[4] = 0xBE;
            actor[5] = 0;
            actor[8] = 0x18;
            actor[9] = 0;
            actor[0xC] = 0;
            actor[0xD] = 0;
            actor[0xE] = 0;
            actor[0xF] = 0;
            object_command_run(actor, 0x80u);
            _check(object_action_pending() == 4u,
                   "the next wind-up charge re-arms event 4 (0x373B)");
            object_pending_run();
            _check(object_charge_tick() == 1, "the second charge loop runs");

            /* D-193: a code the loop's own update_world frame arms is consumed
             * by the loop (0x3705 -> 0x3735/0x373B), not dispatched, and the
             * word store clears the debounce too.  The walking idle's enter
             * sequence (0xEE3B -> 0xEEF7) is the in-loop producer. */
            object_set_action_latched(0);
            actor[4] = 0x8E;
            actor[5] = 0;
            actor[8] = 0x12;
            actor[9] = 0;
            /* (a dispatched code 5 would write [0x1988] = 1 into program 1's
             * cursor; the swallowed one must leave it at 2) */
            object_action_set_program(1u);
            object_action_set_cursor(2u);
            object_action_set_program(2u);
            _check(object_command_run(actor, 0x00u) == OBJ_CMD_HANDLER,
                   "the walking idle runs the enter sequence (0xEE3B)");
            _check(object_action_pending() == 5u,
                   "the enter queues code 5 inside the charge loop");
            _check(object_action_byte(1u) == 2u,
                   "the enter leaves program 1's cursor alone");
            object_pending_run();
            _check(object_action_pending() == 0u && !object_charge_active(),
                   "the charge loop swallows the pending code (0x3735/0x373B)");
            _check(object_action_byte(1u) == 2u,
                   "the swallowed code does not run the code-5 rewind");
            actor[4] = 0xBE;
            actor[5] = 0;
            actor[8] = 0x18;
            actor[9] = 0;
            actor[0xE] = 0;
            actor[0xF] = 0;
            object_command_run(actor, 0x80u);
            _check(object_action_pending() == 4u,
                   "the swallowed code cleared the debounce (0x373B)");
            object_pending_run();
            _check(object_charge_tick() == 1, "the third charge loop runs");
            /* close the third charge through the release path: 0x373B again */
            object_command_run(actor, 0x00u);
            _check(object_charge_tick() == 0,
                   "the third charge's release ends the loop (0x370C)");
        }
    }

    /* 6c5. D-66: the selected cell IS the program cursor (1000:3947 writes it,
     * 1000:3987 reads prog[cursor+1]), so the spear's four icons run four
     * different ops: 3 (pose), 4 (frame 0xBB/0xBE + type 0x17/0x18), 5 (the
     * frame step), 6 (the collision pass). */
    object_init();
    if (level_load(0) == 0) {
        uint8_t *actor = object_slot(0);

        object_set_action_entity(0u);
        object_action_set_cursor(1u);
        actor[0] = (uint8_t)(448u - 8u);
        actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u;
        actor[3] = 0;
        actor[4] = 0x10;
        actor[6] = 0;
        actor[8] = 2;
        actor[9] = 0;
        object_action_run(); /* the pickup: op 2 */
        /* the six groups' cell bytes persist across levels (they are the DS
         * image's), so pin this one before the sequence */
        object_action_set_cursor(1u);
        _check(actor[7] == 1u && statusbar_action_cell(2u) == 1u,
               "the pickup leaves the spear group on cell 1");
        _check(object_program_header(2u) != NULL &&
                   object_program_header(2u)[2] == 3u &&
                   object_program_header(2u)[3] == 4u &&
                   object_program_header(2u)[4] == 5u &&
                   object_program_header(2u)[5] == 6u,
               "group 2's icons are ops 3,4,5,6");
        /* D-113: op 3's gate is CMP AX,2 / JNC, so the type-2 human returns */
        object_action_run(); /* cell 1 = op 3 */
        _check((rd_le_u16(actor + 4)) == 0x0010u &&
                   (rd_le_u16(actor + 8)) == 0x0002u,
               "op 3 is gated out for type 2 (1000:3A20)");
        object_space_action(); /* SPACE: cell 2 */
        _check(statusbar_action_cell(2u) == 2u,
               "SPACE steps the cell to 2 (1000:3947)");
        object_action_run(); /* op 4 on type 2 */
        _check((rd_le_u16(actor + 4)) == 0x0010u &&
                   (rd_le_u16(actor + 8)) == 0x0002u,
               "op 4 is gated out for type 2 (1000:3A08)");
        actor[8] = 1;
        actor[9] = 0;
        object_action_run();
        _check((rd_le_u16(actor + 4)) == 0x00BEu &&
                   (rd_le_u16(actor + 8)) == 0x0018u,
               "cell 2 runs op 4 (frame 0xBE, type 0x18)");
        /* the posed type must be stood up before the next press (1000:392D),
         * then SPACE steps to cell 3 */
        object_space_action();
        object_space_action();
        _check(statusbar_action_cell(2u) == 3u, "SPACE reaches cell 3");
        object_action_run();
        _check((rd_le_u16(actor + 4)) == 0x0112u &&
                   (rd_le_u16(actor + 8)) == 0x0020u,
               "cell 3 runs op 5 (frame 0x112, type 0x20)");
        object_action_set_cursor(1u);
    }

    /* 6c6. D-67: the overlay frame table runs past 0x7F, so the carried record
     * is drawn in the pose frames.  A class record at frame 0xBB (row 0x1F)
     * with class slot 5 (col 1) resolves entry 0x52 = {06,09,60,00}: the
     * carried spear's frame 0x60 at (x+6, y+9); frame 0x8C (row 0x17) resolves
     * entry 0x16 = {FE,FB,5B,79}: frame 0x5B at (x-2, y-5), ybias 4. */
    object_init();
    if (level_load(0) == 0) {
        uint32_t bankSize = 0;
        uint8_t *bank = res_load_chunk("APL\\SPRITES.DAT", &bankSize, NULL);
        uint8_t *actor = object_slot(0);
        static uint8_t page[320u * 200u];

        object_set_action_entity(0u);
        object_action_set_cursor(1u);
        actor[0] = (uint8_t)(448u - 8u);
        actor[1] = (uint8_t)((448u - 8u) >> 8);
        actor[2] = 32u;
        actor[3] = 0;
        actor[4] = 0x10;
        actor[6] = 0;
        actor[8] = 2;
        actor[9] = 0;
        object_action_run(); /* the pickup: class 1, slot 0x85 */
        _check(bank != NULL && bankSize > 0x10A0u + 0x40u,
               "SPRITES.DAT loads for the overlay test");
        if (bank) {
            uint32_t ink = 0, wrong = 0, row, col;

            object_publish_bank(bank, bankSize);
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
            _check(ink > 40u && wrong == 0u,
                   "the carried spear is drawn at (x+6,y+9) in the throw pose");
            /* frame 0x8C (row 0x17) resolves entry 0x16 = {FE,FB,5B,79}: the
             * carried record is moved to (x-2, y-5) with frame 0x5B (the
             * entry's trailing 0x79 sprite is drawn over part of it, so the
             * pixel law is only checked on the throw pose above) */
            actor[4] = 0x8Cu;
            actor[5] = 0;
            memset(page, 0, sizeof page);
            object_render(page, 0, 0);
            _check((rd_le_u16(object_slot(9) + 0)) == 98u &&
                       (rd_le_u16(object_slot(9) + 2)) == 20u &&
                       (rd_le_u16(object_slot(9) + 4)) == 0x005Bu,
                   "the aim pose carries the spear at (x-2,y-5), frame 0x5B");
            /* D-125 (closed 2026-11-05): 1000:6495's second half.  Two
             * frames pin the 0x351D word's axis split: frame 0x10 passes the
             * 0x3541 bit with word 0x0102 (low 2 -> y += 2>>2 = 0, high 1 ->
             * x += 1, so sprite 0x30 at (x+1, y)); frame 0x00's 0x04FC gives
             * low 0xFC -> y += -1 and high 4 -> x += 4 (sprite 0x2E at
             * (x+4, y-1)).  0x6625 takes CX = x and DX = y. */
            object_init();
            {
                static const struct {
                    uint32_t frame, spr, ox;
                    int32_t oy;
                } cases[2] = { { 0x00u, 0x2Eu, 4u, -1 },
                               { 0x10u, 0x30u, 1u, 0 } };
                uint8_t *p = object_slot(0);

                for (uint32_t k = 0; k < 2u; k++) {
                    uint32_t off = 0, rows = 0, ink = 0, wrong = 0;
                    int32_t px = 100 + (int32_t)cases[k].ox;
                    int32_t py = 25 + cases[k].oy;

                    memset(p, 0, 0x12u);
                    _tf_wr16(p, 0u, 100u);
                    _tf_wr16(p, 2u, 25u);
                    _tf_wr16(p, 4u, cases[k].frame);
                    _tf_wr16(p, 8u, 0x0002u);
                    object_set_player(0);
                    object_set_action_entity(0u);
                    memset(page, 0, sizeof page);
                    object_render(page, 0, 0);
                    object_frame_for_probe(cases[k].spr, &off, &rows);
                    _check(off != 0u && rows == 14u,
                           "D-125: the overlay sprite is 14 rows");
                    for (uint32_t r = 0; r < rows; r++)
                        for (uint32_t c = 0; c < 16u; c++) {
                            /* descriptor law: four column blocks of rows*4 */
                            uint8_t v = bank[off + (c & 3u) * rows * 4u +
                                             r * 4u + (c >> 2)];

                            if (!v) continue;
                            ink++;
                            if (page[(uint32_t)(py + (int32_t)r) * 320u +
                                     (uint32_t)(px + (int32_t)c)] != v)
                                wrong++;
                        }
                    _check(ink > 20u && wrong == 0u,
                           "D-125: the overlay is drawn at the frame's offset");
                }
                object_set_player(-1);
            }
            object_publish_bank(NULL, 0u);
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

        for (o = 1; o <= object_live_count() - 8u; o++)
            if (object_slot_class(o) == 4u) {
                ordinal = o;
                break;
            }
        _check(ordinal != 0u, "level 13's 0xAE record takes behaviour 4");
        if (ordinal) {
            uint8_t *it = object_slot(8u + ordinal), *actor = object_slot(0);
            uint32_t ix = rd_le_u16(it + 0), iy = rd_le_u16(it + 2);

            _check((rd_le_u16(it + 0xEu)) == 0xFFFFu,
                   "the 0xAE record carries +0xE = 0xFFFF (1000:52EB)");
            object_set_action_entity(0u);
            object_action_set_cursor(1u);
            actor[0] = (uint8_t)(ix - 8u);
            actor[1] = (uint8_t)((ix - 8u) >> 8);
            actor[2] = it[2];
            actor[3] = it[3];
            actor[4] = 0x10;
            actor[6] = 0;
            actor[8] = 2;
            actor[9] = 0;
            object_action_run(); /* the give / hold */
            _check(
                actor[7] == ordinal && object_slot_class(ordinal) == 0x84u &&
                    (rd_le_u16(actor + 8)) == 0x000Fu,
                "the give attaches the record and makes the actor type 0x0F");
            _check(object_action_program() == 6u &&
                       statusbar_action_group() == 6u,
                   "the give moves the bar to behaviour 4's group 6");
            object_action_run(); /* program 6: op 6 */
            _check(actor[7] == 0u && object_slot_class(ordinal) == 0x04u,
                   "the class path clears the class and the mark");
            _check((uint32_t)(rd_le_u16(actor + 2)) == iy - 8u,
                   "the class path puts the actor at the record's place");
            _check(object_action_program() == 1u &&
                       statusbar_action_group() == 1u,
                   "the drop puts the bar back on group 1");
        }
    }

    /* 6c5. D-237/D-238: level 84's 0xAE wheel (record 12, frame 0x38).
     * Mounting gives the rider type 0x0F and +0xE -> the wheel; the wheel's
     * phase-1 handler must then (a) move x by vx>>3 (1000:D1E3) and (b) reach
     * 1000:D162 so the rider gets the riding frames. */
    if (level_load(83) == 0) {
        uint32_t ordinal = 0;

        for (uint32_t o = 1; o <= object_live_count() - 8u; o++)
            if (object_slot_class(o) == 4u) {
                ordinal = o;
                break;
            }
        _check(ordinal != 0u, "level 84's 0xAE record takes behaviour 4");
        if (ordinal) {
            uint8_t *it = object_slot(8u + ordinal), *actor = object_slot(0);
            uint32_t ix = rd_le_u16(it + 0), iy = rd_le_u16(it + 2);
            uint32_t x0;

            actor[0] = (uint8_t)(ix - 8u);
            actor[1] = (uint8_t)((ix - 8u) >> 8);
            actor[2] = (uint8_t)(iy - 0x10u);
            actor[3] = (uint8_t)((iy - 0x10u) >> 8);
            actor[4] = 0x10;
            actor[6] = 0;
            actor[8] = 2;
            actor[9] = 0;
            object_set_action_entity(0u);
            object_action_set_cursor(1u);
            object_action_set_program(1u);
            object_set_action_latched(0);
            object_action_run(); /* the give / hold */
            _check(rd_le_u16(actor + 8) == 0x000Fu &&
                       object_slot_class(ordinal) == 0x84u,
                   "level 84's wheel mounts (type 0x0F, class 0x84)");
            object_update_list(); /* the idle phase-1 tick */
            _check(rd_le_u16(actor + 4) == 0x0050u,
                   "D-238: the idle rider takes frame 0x50 (1000:D162)");
            x0 = rd_le_u16(it + 0);
            for (uint32_t t = 0; t < 40u; t++) {
                (void)object_command_run(actor, 0x08u); /* right */
                object_update_list();
            }
            _check((uint32_t)rd_le_u16(it + 0) > x0,
                   "D-237: the wheel rolls (1000:D1E3 advances x)");
            _check((uint32_t)rd_le_u16(actor + 0) + 8u ==
                       (uint32_t)rd_le_u16(it + 0),
                   "D-238: the rider follows the wheel (1000:D162)");
            _check(rd_le_u16(it + 4) >= 0x38u && rd_le_u16(it + 4) <= 0x3Bu,
                   "the wheel keeps its 0x38..0x3B roll frames");
        }
    }

    /* 7b5. D-62: render_objects' tail (1000:5F54) blits the BD C block - two
     * 32x24 mode-2 sprites at (x, y) and (x+0x20, y) - for each 0x32B9 entry.
     * Level 5's single 0xA9 is at (352, 257), so with the view at (192,128)
     * the halves land at (160,129) and (192,129). */
    if (level_load(4) == 0) {
        const uint8_t *cb = level_c_block_for_probe();
        static uint8_t flat[320u * 200u];

        _check(cb != NULL, "level 5 has the 0xA9 background art");
        if (cb) {
            uint32_t ink = 0, _bad = 0, s, row, col, i;

            for (i = 0; i < 0x600u; i++)
                if (cb[i]) ink++;
            _check(ink > 300u, "the C block carries the branch art");
            /* level_setup_screen's add_delta_nonzero(0x600, 0x20): every
             * non-zero C byte moves into the 0x20..0x3F art bank (BD2's raw
             * block is 0x18..0x1E). */
            {
                uint32_t out = 0;
                for (i = 0; i < 0x600u; i++)
                    if (cb[i] && (cb[i] < 0x20u || cb[i] > 0x3Fu)) out++;
                _check(out == 0u, "the C block carries the +0x20 art bias");
            }
            /* the draw law, on a blank page at the marker */
            memset(flat, 0, sizeof flat);
            level_draw_bg_sprites(flat, 352, 257);
            for (s = 0; s < 2u; s++)
                for (row = 0; row < 24u; row++)
                    for (col = 0; col < 32u; col++) {
                        uint8_t want = cb[s * 0x300u + (col & 3u) * 0xC0u +
                                          row * 8u + (col >> 2)];

                        if (flat[row * 320u + s * 0x20u + col] != want) _bad++;
                    }
            _check(_bad == 0u, "the C block blits as two 32x24 halves");
            /* and the pass is wired into the frame */
            level_set_view(192u, 128u);
            level_redraw_page();
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
                _check(seen == ink && hit == seen,
                       "the 0xA9 branch art is on the level 5 page");
            }
            level_set_view(0u, 0u);
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
            object_frame_for_probe(frames[k], &off, &rows);
            if (rows != 0u) continue;
            for (i = 8u; i <= object_live_count(); i++) {
                uint8_t *r = object_slot(i);
                if ((uint32_t)(rd_le_u16(r + 4)) == frames[k]) {
                    found++;
                    break;
                }
            }
        }
        _check(found == 3u,
               "level 16 keeps the three decoration records (frames C1/C5/C9)");
        /* the G pass draws them: the pool cells must carry the G bank
         * (0x21..0x3F), where the BG alone is 0x40..0x47 */
        level_set_view(192u, 168u);
        level_redraw_page();
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
        _check(water > 100u, "the DEFS G sprites draw the level 16 water hole");
    }

    /* 7b7. D-72: the A9 log drop.  The settle's 0xDF2C writes 0xFF at
     * firstRecord + 8 + 0x12*k (the tables 0x689E/0x68BE), and 1000:D5E4 then
     * falls that record +2 y per tick and hides it past y 0x138.  Live-
     * verified 2026-11-02 (log 9's +8 = 1 -> (240,276) to (240,314), +6 = 0xFF). */
    if (level_load(15) == 0) {
        uint8_t *log = NULL;
        uint32_t i, col, row;

        for (i = 8u; i <= object_live_count(); i++) {
            uint8_t *r = object_slot(i);

            if ((uint32_t)(rd_le_u16(r + 4)) == 0x33u) {
                log = r;
                break;
            }
        }
        _check(log != NULL, "level 16's first A9 log record exists");
        /* the marker pair lands one cell right of the 0xA9 */
        for (row = 0; row < 20u; row++) {
            for (col = 0; col < 32u; col++)
                if (level_map()[row * 32u + col] == 0xA9u) break;
            if (col < 32u) break;
        }
        _check(row < 20u && level_map()[(row + 1u) * 32u + col] == 0x33u &&
                   level_map()[(row + 1u) * 32u + col + 1u] == 0xC2u &&
                   level_map()[(row + 1u) * 32u + col + 2u] == 0xC9u,
               "the 0xA9 markers sit one cell right of the tile (live)");
        if (log) {
            uint8_t *logs[5];
            uint32_t k;

            for (k = 0; k < 5u; k++) logs[k] = log + k * 0x12u;
            /* the marker pair, as 1000:DF2C leaves it */
            _check(level_map()[(row + 1u) * 32u + col + 1u] == 0xC2u &&
                       level_map()[(row + 1u) * 32u + col + 2u] == 0xC9u,
                   "the A9 marker pair is C2/C9 before a mark");
            /* the settle pokes the log the foot is on: the table offset is
             * 8 + 0x12*k, i.e. that record's +8 */
            logs[4][8] = 0xFFu; /* log 13, the rightmost */
            logs[4][9] = 0xFFu;
            object_tick(NULL);
            _check((uint32_t)(rd_le_u16(logs[4] + 2)) == 0x116u &&
                       !(logs[4][6] & 0x80u),
                   "a marked log falls 2 px on the next tick");
            /* live: after log 13 the pair reads C2/CA (DS:0x6697's table) */
            _check(level_map()[(row + 1u) * 32u + col + 1u] == 0xC2u &&
                       level_map()[(row + 1u) * 32u + col + 2u] == 0xCAu,
                   "the marker pair encodes the marked log (C2/CA)");
            /* live: logs 11..13 marked -> C3/00 */
            logs[2][8] = logs[2][9] = 0xFFu;
            logs[3][8] = logs[3][9] = 0xFFu;
            object_tick(NULL);
            _check(level_map()[(row + 1u) * 32u + col + 1u] == 0xC3u &&
                       level_map()[(row + 1u) * 32u + col + 2u] == 0x00u,
                   "the marker pair follows the marked set (C3/00)");
            /* past 0x138 the record hides itself */
            logs[4][2] = 0x38u;
            logs[4][3] = 0x01u; /* y = 0x138 */
            object_tick(NULL);
            _check((uint32_t)(rd_le_u16(logs[4] + 2)) == 0x13Au &&
                       (logs[4][6] & 0x80u) != 0u,
                   "past y 0x138 the log hides itself (1000:D5E4)");
        }
    }

    /* 7b7b. D-136: the settle's log mark.  `1000:DF2C` indexes the
     * 0x689E/0x68BE table with `d = foot_x - ((rx+0x10) & 0xFFF0)` (the
     * offset from the 16-aligned window start, `0xDFB1..0xDFE2`), and a
     * static record uses 0x689E unless its type is exactly 1
     * (`0xE001..0xE009`).  Level 16's A9 sits at (224,257), so the window
     * starts at (240,272); the foot at (246,272) is d = 6, where
     * `0x689E[6] = 8` (log 0) and `0x68BE[6] = 0x1A` (log 1). */
    if (level_load(15) == 0) {
        for (uint32_t type = 0; type < 2u; type++) {
            uint8_t *logs[5] = { NULL, NULL, NULL, NULL, NULL };
            uint8_t *a;
            uint32_t i, k, n;

            object_init();
            n = object_load_terrain_objects((uint8_t *)level_map());
            for (i = 8u; i <= n; i++) {
                uint8_t *r = object_slot(i);
                uint32_t f = (uint32_t)rd_le_u16(r + 4);

                if (f >= 0x33u && f <= 0x37u) logs[f - 0x33u] = r;
            }
            _check(logs[0] != NULL && logs[4] != NULL,
                   "the A9 log fixtures exist for the settle mark");
            if (!logs[0] || !logs[4]) continue;
            for (k = 0; k < 5u; k++) logs[k][8] = logs[k][9] = 0u;
            a = object_slot(0);
            memset(a, 0, 0x12u);
            _tf_wr16(a, 0, 238u); /* foot x = 246 */
            _tf_wr16(a, 2, 248u); /* foot y = 272 (before the settle) */
            a[4] = 0x10u;
            a[8] = (uint8_t)type;
            object_set_action_entity(0u);
            _check(object_command_run(a, 0x80u) == OBJ_CMD_TAIL,
                   "the settle hook runs for the log mark");
            _check((uint32_t)(rd_le_u16(a + 2)) == 256u,
                   "the settle snaps the actor onto the log row");
            if (type == 0u) {
                _check(logs[0][8] == 0xFFu && logs[1][8] == 0u,
                       "D-136: type 0 marks log 0 via 0x689E[d]");
            } else {
                _check(logs[1][8] == 0xFFu && logs[0][8] == 0u,
                       "D-136: type 1 marks log 1 via 0x68BE[d]");
            }
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
                _check(ink > 10u,
                       "the A9 log sprite draws (field E, frame 0x33)");
            else
                _check(ink > 10u, k == 1u ? "field H draws frame 0x26"
                                          : "field H draws frame 0x27");
        }
    }

    /* 7b9. D-74: the placed-record behaviour walk (1000:C295).  Level 16's
     * torch (behaviour 6) steps its frame every 4th frame; the decorations
     * (behaviour 1) reload their +0xA countdown on the 8-tick cadence. */
    if (level_load(15) == 0) {
        uint8_t *torch = NULL, *deco = NULL;
        uint32_t i, f0, seen = 0;

        for (i = 8u; i <= object_live_count(); i++) {
            uint8_t *r = object_slot(i);

            if ((rd_le_u16(r + 4)) == 0x9Cu) torch = r;
            if ((rd_le_u16(r + 4)) == 0xC1u) deco = r;
        }
        _check(torch != NULL && deco != NULL,
               "level 16 carries the torch and a behaviour-1 decoration");
        if (torch && deco) {
            uint32_t idx = (uint32_t)((torch - object_slot(0)) / 0x12u);

            _check((uint32_t)(object_slot_class(idx - 8u) & 0x3Fu) == 6u,
                   "level 16's torch is behaviour 6");
            f0 = rd_le_u16(torch + 4);
            for (i = 0; i < 9u; i++) {
                object_update_list();
                object_tick(NULL);
                seen |= 1u << (uint32_t)((rd_le_u16(torch + 4)) - f0);
            }
            _check(seen == 0x7u, "the torch walks 0x9C..0x9E (behaviour 6)");
            /* D-88: 1000:E039's settle landed the torch on its two-pixel step:
             * y 177 -> 178 (the live record's value, D-76) with +0xA/+0xE
             * cleared, so the frame walk above is not interrupted. */
            _check((rd_le_u16(torch + 2)) == 178u &&
                       (rd_le_u16(torch + 10)) == 0u &&
                       (rd_le_u16(torch + 14)) == 0u,
                   "1000:E039 settles the torch at y = 178 (D-88)");
            {
                uint8_t *b1 = NULL;
                uint32_t j;

                for (j = 8u; j <= object_live_count(); j++) {
                    uint8_t *q = object_slot(j);

                    if ((uint32_t)(object_slot_class(j - 8u) & 0x3Fu) == 1u) {
                        b1 = q;
                        break;
                    }
                }
                if (b1) {
                    object_update_list();
                    _check((rd_le_u16(b1 + 10)) != 0u,
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
        (void)object_load_terrain_objects(m);
        for (i = 8u; i <= object_live_count(); i++) {
            uint8_t *r = object_slot(i);
            uint32_t cls = object_slot_class(i - 8u) & 0x3Fu;

            if (cls == 5u && !mover) mover = r;
            if (cls == 8u && !rec) rec = r;
        }
        slot = object_slot(0);
        _check(mover && slot && rec, "the mover/slot/record fixtures spawn");
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
            object_update_list();
            _check((rd_le_u16(slot + 4)) == 0x0028u &&
                       (rd_le_u16(slot + 8)) == 0x0016u &&
                       (rd_le_u16(slot + 0xAu)) == 5u &&
                       (rd_le_u16(slot + 0)) == 92u && save[6] == 0xFFu,
                   "a mover in reach rewrites the placed slot (1000:5B3E)");
            (void)keep;
        }
        if (mover && rec) {
            object_set_slot_class_for_probe(
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
            object_update_list();
            _check(
                (rd_le_u16(rec + 4)) == 0x0028u && (rd_le_u16(rec + 8)) == 2u &&
                    (rd_le_u16(rec + 0xAu)) == 6u && mover[6] == 0xFFu,
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
        (void)object_load_terrain_objects(m);
        for (i = 8u; i <= object_live_count(); i++) {
            uint8_t *r = object_slot(i);

            if ((object_slot_class(i - 8u) & 0x3Fu) == 8u) {
                rec = r;
                break;
            }
        }
        _check(rec != NULL, "the 0xB7 behaviour-8 record spawns");
        if (rec) {
            rec[0xA] = 2u; /* exercise the state-2 animation directly */
            rec[0xB] = 0u;
            f0 = rd_le_u16(rec + 4);
            object_update_list();
            _check((uint32_t)(rd_le_u16(rec + 4)) == f0 + 1u &&
                       (rd_le_u16(rec + 8)) == 6u,
                   "the state-2 animation advances the frame (1000:C615)");
        }
    }

    /* 7b11b. D-140: both landing probes add the record's coordinates before
     * the 0xE389 lookup (`0xE361` does `ADD AX,[SI] / ADD BX,[SI+2]`), so
     * 0xC62C probes (x + vy + 8, y + 0x18) and 0xF08D probes
     * (x - 8, y + 0x18). */
    if (level_load(10u) == 0) {
        static uint8_t m[640];
        uint8_t *rec = NULL;
        uint32_t i, cell;
        int32_t want;

        memset(m, 0, sizeof m);
        m[10u * 32u + 10u] = 0xB7u; /* a behaviour-8 record */
        object_init();
        (void)object_load_terrain_objects(m);
        for (i = 9u; i <= object_live_count(); i++) {
            if ((object_slot_class(i - 8u) & 0x3Fu) == 8u) {
                rec = object_slot(i);
                break;
            }
        }
        _check(rec != NULL, "the behaviour-8 record exists for the probe");
        if (rec) {
            rec[0xA] = 0u; /* state 0: the 0xC62C walk */
            rec[0xB] = 0u;
            want = object_land_y_for_probe(
                (int32_t)(int16_t)rd_le_u16(rec + 0u) + 1 + 8,
                (int32_t)(int16_t)rd_le_u16(rec + 2u) + 0x18, &cell);
            object_update_list();
            _check(want >= 0 &&
                       (int32_t)(int16_t)rd_le_u16(rec + 2u) == want - 0x18,
                   "D-140: the behaviour-8 probe adds the record's y");
        }

        /* the placement tail 0xF08D (type 4's row, position 7), on a spot
         * whose landing is 7 px below the feet row */
        object_init();
        rec = object_slot(0);
        memset(rec, 0, 0x12u);
        _tf_wr16(rec, 0, 201u);
        _tf_wr16(rec, 2, 1u);
        rec[4] = 0x10u;
        rec[8] = 4u;
        object_set_action_entity(0u);
        want = object_land_y_for_probe(201 - 8, 1 + 0x18, &cell);
        _check(want >= 0 && want - 0x18 - 1 >= 0 && want - 0x18 - 1 < 8,
               "the 0xF08D placement fixture lands 0..7 below the row");
        (void)object_command_run(rec, 0x04u);
        _check((uint32_t)(rd_le_u16(rec + 4u)) == 8u &&
                   (uint32_t)(rd_le_u16(rec + 8u)) == 1u,
               "D-140: 0xF08D lands on (x - 8, y + 0x18)");

        /* D-124: the shared ground snap stores y = land - 0x10
         * unconditionally (1000:5639), so a blocked probe parks the spear
         * at 0xFFEF instead of leaving it on the map.  Level 11's (0,0)
         * cell is solid above and below the probe row. */
        memset(m, 0, sizeof m);
        m[0] = 0xABu;
        object_init();
        _check(object_load_terrain_objects(m) == 9u,
               "the blocked 0xAB appends the spear");
        rec = object_slot(9);
        _check(rec != NULL && (uint32_t)(rd_le_u16(rec + 2u)) == 0xFFEFu,
               "D-124: a blocked probe parks the spear at y = 0xFFEF");
    }

    /* D-147: 1000:F2E0 dissolves the group of the record it is about to
     * convert (`0xF453` with DX = 5, inside the walk), not the actor's, and
     * only when an overlap is found. */
    {
        uint8_t *a, *c;

        /* (a) no candidate: the actor's own group survives */
        object_init();
        a = object_slot(0);
        memset(a, 0, 0x12u);
        _tf_wr16(a, 0u, 300u);
        _tf_wr16(a, 2u, 100u);
        a[4] = 0x10u;
        a[8] = 2u;
        object_set_action_entity(0u);
        object_action_set_program(1u);
        object_action_set_cursor(2u); /* program 1, cell 2 = op 1 */
        object_action_run(); /* registers record 0 as a group owner */
        object_find_overlap_for_probe(a);
        _check(object_stack_scan_dissolve(0u) == 1,
               "D-147: no overlap leaves the actor's group alone");

        /* (b) an overlapping candidate's own group is dissolved */
        object_init();
        a = object_slot(0);
        memset(a, 0, 0x12u);
        _tf_wr16(a, 0u, 100u);
        _tf_wr16(a, 2u, 100u);
        a[4] = 0x10u;
        a[8] = 2u;
        c = object_slot(2);
        memset(c, 0, 0x12u);
        _tf_wr16(c, 0u, 105u);
        _tf_wr16(c, 2u, 100u);
        c[4] = 0x10u;
        c[8] = 2u;
        object_set_action_entity(2u);
        object_action_set_program(1u);
        object_action_set_cursor(2u);
        object_action_run(); /* registers record 2 as a group owner */
        c[6] = 0u;
        object_find_overlap_for_probe(a);
        _check((rd_le_u16(c + 8u)) == 0x1Eu && (rd_le_u16(c + 4u)) == 0x103u,
               "D-147: the overlapping candidate converts");
        _check(object_stack_scan_dissolve(2u) == 0,
               "D-147: the candidate's group was dissolved");
    }

    /* D-187: 1000:F4D9..0xF5BB, the class-transfer tail of 0xF453 (every
     * caller passes DX = 5). */
    {
        static uint8_t m[640];
        uint8_t *item, *rec;

        /* the item behaviour: the carried live record is dropped at the
         * carrier (behaviour 5 -> frame 0x60) */
        memset(m, 0, sizeof m);
        m[0] = 0xABu; /* appends the spear at live index 9 */
        object_init();
        (void)object_load_terrain_objects(m);
        item = object_slot(9);
        rec = object_slot(1);
        _check(item != NULL && rec != NULL, "D-187: the live spear fixture");
        if (item && rec) {
            memset(item, 0, 0x12u);
            _tf_wr16(item, 0u, 10u);
            _tf_wr16(item, 2u, 20u);
            _tf_wr16(item, 4u, 0x0060u);
            _tf_wr16(item, 8u, 0x1234u);
            memset(rec, 0, 0x12u);
            _tf_wr16(rec, 0u, 50u);
            _tf_wr16(rec, 2u, 60u);
            _tf_wr16(rec, 4u, 0x0010u);
            _tf_wr16(rec, 8u, 2u);
            rec[7] = 1u; /* class 1 = live index 9 */
            object_set_slot_class(1u, 5u);
            object_set_action_entity(1u);
            (void)object_stack_scan_dissolve(1u);
            _check(rec[7] == 0u, "D-187: the class is cleared");
            _check((uint32_t)(rd_le_u16(item + 0u)) == 50u &&
                       (uint32_t)(rd_le_u16(item + 2u)) == 68u &&
                       (uint32_t)(rd_le_u16(item + 4u)) == 0x0060u &&
                       (uint32_t)(rd_le_u16(item + 6u)) == 0u &&
                       (uint32_t)(rd_le_u16(item + 8u)) == 0xFFFFu,
                   "D-187: the carried spear is dropped at the carrier");
            _check((object_slot_class(1u) & 0x3Fu) == 5u &&
                       (object_slot_class(1u) & 0x80u) == 0u,
                   "D-187: the class slot's taken mark is cleared");
            _check(object_action_program() == 1u &&
                       object_action_pending() == 5u,
                   "D-187: the acting record's event 5 is queued");
        }

        /* the behaviour-4 slot: the live projectile is lifted into the
         * record (bit 15 of live+8 -> frame 8 / type 1) */
        memset(m, 0, sizeof m);
        m[0] = 0xAEu; /* the stone, live index 9, behaviour 4 */
        object_init();
        (void)object_load_terrain_objects(m);
        item = object_slot(9);
        rec = object_slot(1);
        _check(item != NULL && rec != NULL, "D-187: the live stone fixture");
        if (item && rec) {
            memset(item, 0, 0x12u);
            _tf_wr16(item, 0u, 100u);
            _tf_wr16(item, 2u, 100u);
            _tf_wr16(item, 8u, 0x8000u);
            item[6] = 0xFFu;
            _tf_wr16(item, 0xEu, 0x1111u);
            memset(rec, 0, 0x12u);
            _tf_wr16(rec, 4u, 0x0010u);
            _tf_wr16(rec, 8u, 2u);
            rec[7] = 1u;
            object_set_slot_class(1u, 4u);
            object_set_action_entity(1u);
            (void)object_stack_scan_dissolve(1u);
            _check((uint32_t)(rd_le_u16(item + 0xEu)) == 0xFFFFu &&
                       (item[6] & 0x80u) == 0u,
                   "D-187: the behaviour-4 live record is consumed");
            _check((uint32_t)(rd_le_u16(rec + 0u)) == 100u &&
                       (uint32_t)(rd_le_u16(rec + 2u)) == 92u &&
                       (uint32_t)(rd_le_u16(rec + 4u)) == 8u &&
                       (uint32_t)(rd_le_u16(rec + 8u)) == 1u,
                   "D-187: the record takes the live projectile's form");
        }
    }

    /* D-188: the 0x6EC3 gate's full DS window (the image's 0x5F-byte run,
     * the live 0x6F22 pool and the 0x6F92 spans). */
    {
        uint8_t *a, *c;

        object_init();
        _check(object_type_blocked(2u) == 0 && object_type_blocked(3u) == 1 &&
                   object_type_blocked(8u) == 0 &&
                   object_type_blocked(0x16u) == 1 &&
                   object_type_blocked(0x1Bu) == 1 &&
                   object_type_blocked(0x1Du) == 1 &&
                   object_type_blocked(0x1Eu) == 1 &&
                   object_type_blocked(0x21u) == 1 &&
                   object_type_blocked(0x2Du) == 1,
               "D-188: the image's 0x6EC3 run");
        _check(object_type_blocked(0xC1u) == 1,
               "D-188: the free register pool blocks a decoration type");
        a = object_slot(0);
        memset(a, 0, 0x12u);
        _tf_wr16(a, 0u, 0u); /* pool word 0's low byte = type 0x5F */
        _tf_wr16(a, 2u, 10u);
        a[4] = 0x10u;
        a[8] = 2u;
        object_set_action_entity(0u);
        object_action_set_program(1u);
        object_action_set_cursor(2u);
        object_action_run(); /* op 1 registers it: pool byte 0x5F = 0 */
        _check(object_type_blocked(0x5Fu) == 0,
               "D-188: type 0x5F reads the live register pool byte");

        /* an overlapping type-0x1D candidate is skipped, not converted */
        object_init();
        a = object_slot(0);
        memset(a, 0, 0x12u);
        _tf_wr16(a, 0u, 100u);
        _tf_wr16(a, 2u, 100u);
        a[4] = 0x10u;
        a[8] = 2u;
        c = object_slot(2);
        memset(c, 0, 0x12u);
        _tf_wr16(c, 0u, 105u);
        _tf_wr16(c, 2u, 100u);
        c[4] = 0x10u;
        c[8] = 0x1Du;
        object_find_overlap_for_probe(a);
        _check((uint32_t)(rd_le_u16(c + 8u)) == 0x1Du &&
                   (uint32_t)(rd_le_u16(c + 4u)) != 0x103u,
               "D-188: a type-0x1D candidate is not converted");
    }

    /* 7b12. D-74: behaviour 4's projectile (1000:CEC3).  Level 13's 0xAE
     * stone is behaviour 4 with vx = vy = 0; phase 1 reads the field-A slope
     * and moves x (or starts the fall via +0xA). */
    if (level_load(12) == 0) {
        uint8_t *stone = NULL;
        uint32_t i, x0, a0;

        for (i = 8u; i <= object_live_count(); i++) {
            if ((object_slot_class(i - 8u) & 0x3Fu) == 4u) {
                stone = object_slot(i);
                break;
            }
        }
        _check(stone != NULL, "level 13's 0xAE stone is behaviour 4");
        if (stone) {
            /* phase 2: set vy = 1 and _check the 0x6652 curve (-4) and the
             * vy++ step (the phase-1 slope is not guaranteed on this cell) */
            x0 = (uint32_t)(rd_le_u16(stone + 0));
            a0 = (uint32_t)(rd_le_u16(stone + 2));
            stone[0xA] = 1;
            stone[0xB] = 0;
            object_update_list();
            _check((uint32_t)(rd_le_u16(stone + 2)) != a0 &&
                       (rd_le_u16(stone + 0xA)) == 2u,
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
        object_set_action_entity(0u);
        object_action_set_program(2u);
        object_action_set_cursor(2u); /* program 2, cell 2 = icon 4 */
        object_set_action_latched(0);
        object_set_slot_class(1u, 5u);
        a[7] = 1u;
        a[0] = 100;
        a[1] = 0;
        a[2] = 100;
        a[3] = 0;
        a[4] = 0x10;
        a[5] = 0;
        a[6] = 0;
        a[8] = 1; /* op 4's gate is CMP AX,2 / JNC (D-113) */
        a[9] = 0;
        a[0xA] = 0;
        a[0xB] = 0;
        a[0xC] = 0;
        a[0xD] = 0;
        a[0xE] = 0;
        a[0xF] = 0;

        object_action_run(); /* the tap */
        _check(
            (rd_le_u16(a + 8)) == 0x0018u && (rd_le_u16(a + 4)) == 0x00BEu &&
                object_action_latched(),
            "op 4 winds the human up and pause_toggle latches (1000:EFA1/0x3538)");

        object_set_action_latched(0); /* the tap's release */
        _check(!object_action_latched(),
               "the release clears the latch (1000:85A9)");

        (void)object_command_run(a, 0x80u); /* a held frame: the fire column */
        _check(!object_action_latched() &&
                   (uint32_t)(rd_le_u16(a + 0xE)) == 4u &&
                   object_action_pending() == 4u,
               "the fire column charges without latching (1000:EE07)");
        object_pending_run();
        _check(object_charge_tick() == 1,
               "the charge loop draws the bar (1000:3702)");

        (void)object_command_run(a, 0x80u); /* the next held frame */
        _check((uint32_t)(rd_le_u16(a + 0xE)) == 8u,
               "the charge keeps walking +0xE while ACTION is held");
        object_action_run(); /* 1000:3530: blocked by [0x6F1D] */
        _check((rd_le_u16(a + 8)) == 0x0018u,
               "pause_toggle is blocked while the charge owns the record");
        /* 1000:3702 reuses the press frame's [0x5ACE]; a direction pressed
         * mid-charge must not replace the row half the loop runs. */
        (void)object_command_run(a, 0x84u); /* fire + LEFT */
        _check((uint32_t)(rd_le_u16(a + 0xE)) == 0xCu &&
                   (uint32_t)(rd_le_u16(a + 8)) == 0x0018u,
               "the charge loop reuses the press input, not the live "
               "direction");

        (void)object_command_run(a, 0x00u); /* the release */
        {
            uint32_t t = rd_le_u16(a + 8);
            uint32_t e = rd_le_u16(a + 0xE);

            /* 0xEFE4 bumps the type by 2 (0x18 -> 0x1A) and runs the fall
             * machine, which may land straight into another state; the one
             * thing the release must do is leave the wind-up - nothing else
             * in the type-0x18 no-fire column changes the type. */
            if (t == 0x0018u)
                fprintf(stderr, "  (release: type %04x +E %04x)\n", t, e);
            _check(t != 0x0018u,
                   "the release leaves the wind-up (1000:EFD7 -> 0xEFE4)");
            /* D-84: the landing pair is 1000:F8D2/F8D8 - frame <- [0x4230]
             * (objSide), type <- [0x4234] (objSelXoff) - so the throw path
             * (0xF078: 8/1) must not land as frame 1/type 0. */
            _check((t == 0x0000u || t == 0x0001u) &&
                       ((rd_le_u16(a + 4)) == 8u || (rd_le_u16(a + 4)) == 0u),
                   "the landing pair is frame <- [0x4230], type <- [0x4234]");
        }
    }

    /* 7b4. D-63: the sprite table runs to the 0x3E20 bound - 0x11E slots.
     * apply_relocations builds it from 0x39A8 to 0x3E20; the 0xB7 bush and
     * the type-31/32 tails use frames 0x116/0x11A, which a 0x110 table drops. */
    _check(object_frame_slots() == 0x11Eu,
           "the sprite table holds 0x11E slots");
    {
        uint32_t sz = 0;
        uint8_t *bank = res_load_chunk("APL\\SPRITES.DAT", &sz, NULL);
        static uint8_t page[320u * 200u];

        _check(bank != NULL, "SPRITES.DAT loads for the frame test");
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
                uint32_t k, _bad = 0;

                for (k = 0; k < 4u; k++) {
                    uint32_t so = 0, sr = 0, dof = 0, dr = 0;

                    object_frame_for_probe(pairs[k][0], &so, &sr);
                    object_frame_for_probe(pairs[k][1], &dof, &dr);
                    if (sr == 0u || dof != so || dr != sr) _bad++;
                }
                _check(_bad == 0u,
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
                object_publish_bank(bank, sz);
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
                _check(k < 2u ? ink > 100u : ink == 0u, whats[k]);
            }
        }
        free(bank);
    }

    /* 7b3. D-61: the 0xB9 exit/igloo pass.  Level 133 has one at col 11 row 9;
     * the BD P block is stamped into the background at (176, 96), 64x64,
     * zero transparent, with level_setup_screen's add_delta_nonzero(0x1000,
     * 0x20) palette move. */
    if (level_load(132) == 0) {
        const uint8_t *bg = level_bg_for_probe();
        const uint8_t *pb = level_p_block_for_probe();
        uint32_t ink = 0, _bad = 0;

        _check(bg && pb, "level 133 has the exit/igloo art");
        if (bg && pb) {
            for (uint32_t r = 0; r < 64u; r++)
                for (uint32_t c = 0; c < 64u; c++) {
                    uint8_t v = pb[r * 64u + c];

                    if (!v) continue;
                    ink++;
                    if (bg[(96u + r) * 512u + 176u + c] != (uint8_t)(v + 0x20u))
                        _bad++;
                }
            _check(ink > 500u && _bad == 0u,
                   "the 0xB9 igloo is stamped at (176,96)");
        }
    }
    /* a level with no 0xB9 must not get a stamp: load its bg and _check it is
     * the plain transformed BG file */
    if (level_load(42) == 0) {
        char name[32];
        uint32_t sz = 0;
        uint8_t *raw;
        uint32_t _bad = 0;

        snprintf(name, sizeof name, "BACKS\\BG%u.RAW",
                 level_bg_index_for_probe());
        raw = res_load_chunk(name, &sz, NULL);
        if (raw && sz == 512u * 320u + 768u) {
            const uint8_t *bg = level_bg_for_probe();
            for (uint32_t i = 0; i < 512u * 320u; i++) {
                uint8_t v = raw[i];
                uint8_t want = (v == 0xFFu)
                                   ? (uint8_t)(0x80u + (i / 0xA00u) % 64u)
                                   : (uint8_t)(v < 0x40u ? 0x40u + v : v);

                if (!bg || bg[i] != want) _bad++;
            }
            _check(_bad == 0u, "a level with no 0xB9 keeps its background");
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
            _check(ii == 0xFFu || ii < 0x80u,
                   "the header intro byte is 0xFF/0..9");
            if (gameflow_post_level_scene(1, ii) != 0xFFFFu) newspapers++;
        }
        _check(loaded == 160u, "all 160 level records load");
        _check(newspapers == 14u, "14 levels carry a post-level newspaper");
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
        for (uint32_t i = 0; i < object_static_slots(); i++) {
            uint8_t *r = object_slot(i);

            if (r) r[6] |= 0x80u; /* all inactive */
        }
        statusbar_set_tribe_digits(1u, 2u); /* the tribe counter shows 12 */
        _check(score_members() == 12u,
               "the member count is the tribe counter plus the live records");
        {
            uint8_t *r0 = object_slot(0u);

            if (r0) r0[6] &= 0x7Fu; /* one active record */
            _check(score_members() == 13u, "an active record adds one");
            if (r0) r0[6] |= 0x80u;
        }
        _check(score_best_for(1u) == 0x0999u,
               "the best-time table ships as the 0x0999 sentinel");
        before = (uint32_t)score_total_lo() | ((uint32_t)score_total_hi() << 16);
        level_set_members(3u); /* a stale count the recount overwrites */
        score_prepare();
        _check(level_members() == 12u,
               "1000:41E6: the screen stores the recount into [0x11FC]");
        _check(score_is_record() == 1 &&
                   score_best_for(1u) == statusbar_elapsed_word() &&
                   score_best_time() == statusbar_elapsed_word(),
               "the first win beats the sentinel and stores its elapsed time");
        _check(score_shown_lo() == (before & 0xFFFFu) &&
                   score_shown_hi() == (before >> 16),
               "the displayed total starts at the old running total");
        t1 = (uint32_t)score_total_lo() | ((uint32_t)score_total_hi() << 16);
        _check(t1 > before, "the running total keeps the level's two terms");
        score_prepare(); /* the same elapsed time */
        _check(score_is_record() == 0,
               "an equal (not better) time is not a new record");
        _check(((uint32_t)score_total_lo() | ((uint32_t)score_total_hi() << 16)) > t1,
               "a second level adds its terms again");
        {
            uint32_t guard = 0;

            while (score_counters_run() && guard++ < 20000u) {}
            _check(guard < 20000u && score_shown_lo() == score_total_lo() &&
                       score_shown_hi() == score_total_hi(),
                   "1000:475D counts the display up to the running total");
        }

        /* D-146: [0x1DCA] is not cleared by score_prepare; only the count-up
         * drains it, so a screen left early carries its remainder into the
         * next level's term. */
        {
            uint16_t t0, t2;
            uint32_t guard = 0;

            score_set_total(0, 0);
            score_prepare();
            t0 = score_level_term_for_probe();
            _check(t0 >= 10u, "the score term has at least one 10-point step");
            for (uint32_t i = 0; i < 0x50u; i++)
                (void)score_counters_run(); /* the 0x50-frame delay */
            (void)score_counters_run(); /* one 10-point step */
            _check(score_level_term_for_probe() == (uint16_t)(t0 - 10u),
                   "1000:475D drains [0x1DCA] 10 at a time");
            score_prepare();
            t2 = score_level_term_for_probe();
            _check(t2 == (uint16_t)(t0 + t0 - 10u),
                   "1000:41AE does not clear [0x1DCA]: the leftover carries");
            while (score_counters_run() && guard++ < 20000u) {}
        }
    }

    /* 8b. HISCORES.HUM (re_formats.md §11 / menu_loop 1000:012A): the
     * obfuscation is an involution, the shipped table is the 0x0999 bests and
     * the ten defaults, the insert shifts 18-byte entries, and the name entry
     * follows the grid's 0x1C/0x1D columns. */
    {
        uint8_t buf[600];
        uint8_t len = 0;
        char nameBuf[17];
        char *name = nameBuf + 1; /* one byte before name[0] (0x756) */
        int32_t slot;

        for (uint32_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)i;
        hiscore_obfuscate(buf, sizeof buf);
        {
            int32_t changed = 0;
            for (uint32_t i = 0; i < sizeof buf; i++)
                if (buf[i] != (uint8_t)i) changed = 1;
            _check(changed, "1000:06DE obfuscates the table");
        }
        hiscore_obfuscate(buf, sizeof buf);
        {
            int32_t same = 1;
            for (uint32_t i = 0; i < sizeof buf; i++)
                if (buf[i] != (uint8_t)i) same = 0;
            _check(same, "1000:06DE is an involution (load and save share it)");
        }

        /* D-102: the blob is the original's 0x208 = 520 bytes; level N is at
         * +2*N (1-based), the reserved word at +0, the entries at +0x154. */
        hiscore_reset();
        _check(hiscore_table_size() == 520u,
               "the hi-score blob is 520 bytes (D-102)");
        hiscore_set_best(1u, 0x1234u);
        hiscore_set_best(160u, 0x5678u);
        /* D-225: the original's CD probe stamps the drive letter over the 'H'
         * ("<letter>ISCORES.HUM"); the port keeps the intended HISCORES.HUM
         * and that difference is accepted.  Clear both names first so the
         * check below proves which one the writer creates. */
        remove("DISCORES.HUM");
        remove("HISCORES.HUM");
        hiscore_write();
        {
            FSSave *f = g_osys_null.fs->openNamed("HISCORES.HUM");
            uint8_t raw[600];
            uint8_t enc[600];
            uint32_t n = 0;

            if (f) {
                n = g_osys_null.fs->saveRead(f, 0, raw, sizeof raw);
                g_osys_null.fs->closeSave(f);
            }
            _check(n == 520u, "the hi-score file is 520 bytes (D-102)");
            if (n >= 322u) {
                /* D-120: 1000:06CC has no restore pass, so the runtime block
                 * still holds the file's ciphertext. */
                memcpy(enc, raw, n);
                _check(hiscore_best(1u) == rd_le_u16(enc + 2u) &&
                           hiscore_score(0u) == rd_le_u32(enc + 0x154u) &&
                           memcmp(hiscore_name(0u), enc + 0x158u, 14u) == 0 &&
                           hiscore_score(0u) != 100000u,
                       "D-120: 1000:06CC leaves the table encrypted");
                hiscore_obfuscate(raw, n);
                _check(raw[0] == 0x99u && raw[1] == 0x09u,
                       "the reserved word sits at +0 (D-102)");
                _check(raw[2] == 0x34u && raw[3] == 0x12u,
                       "level 1's u16 sits at +2 (D-102)");
                _check(raw[320] == 0x78u && raw[321] == 0x56u,
                       "level 160's u16 sits at +0x140 (D-102)");
                _check(rd_le_u32(raw + 0x154u) == 100000u,
                       "the ten 18-byte entries start at +0x154 (D-102)");
            }
        }
        {
            FSSave *stamped = g_osys_null.fs->openNamed("DISCORES.HUM");

            if (stamped) g_osys_null.fs->closeSave(stamped);
            _check(
                stamped == NULL,
                "D-225: the port keeps HISCORES.HUM (the original's stamped name is accepted, not reproduced)");
        }

        hiscore_reset();
        _check(hiscore_best(1u) == 0x0999u && hiscore_best(160u) == 0x0999u,
               "every per-level best ships as 0x0999");
        _check(hiscore_score(0u) == 100000u &&
                   strcmp(hiscore_name(0u), "STEVE JONES") == 0,
               "the shipped top-10 is the image's 0x4DE block");

        /* 1000:01CD is a memory insert; the only flush is the MAP exit's
         * 1000:06CC (map.c's _map_close).  The file written above must be
         * byte-identical after an insert. */
        {
            uint8_t before[520], after[520];
            uint32_t n0 = 0, n1 = 0;
            FSSave *f;

            f = g_osys_null.fs->openNamed("HISCORES.HUM");
            if (f) {
                n0 = g_osys_null.fs->saveRead(f, 0, before, sizeof before);
                g_osys_null.fs->closeSave(f);
            }
            _check(hiscore_insert(150000u, "NO FLUSH") == 0,
                   "the insert still takes the top slot");
            f = g_osys_null.fs->openNamed("HISCORES.HUM");
            if (f) {
                n1 = g_osys_null.fs->saveRead(f, 0, after, sizeof after);
                g_osys_null.fs->closeSave(f);
            }
            _check(n0 == 520u && n1 == n0 && memcmp(before, after, n0) == 0,
                   "1000:01CD is memory-only (no flush before 0x06CC)");
        }
        hiscore_reset();

        _check(!hiscore_qualifies(9999u) && hiscore_qualifies(10000u),
               "a score equal to the last entry still displaces it");

        /* 1000:0475/0x0486: Return with no letters commits the zeroed
         * 14-byte buffer, so the table takes an empty name. */
        {
            int32_t e = hiscore_insert(120000u, "");

            _check(e == 0 && hiscore_name(0u)[0] == 0,
                   "1000:0475: Return with no letters inserts an empty name");
        }
        hiscore_reset();
        slot = hiscore_insert(55000u, "AB CD");
        _check(slot == 5, "55000 lands between 60000 and 50000");
        _check(hiscore_score(5u) == 55000u &&
                   strcmp(hiscore_name(5u), "AB CD") == 0,
               "the inserted entry carries its score and name");
        _check(hiscore_score(6u) == 50000u && hiscore_score(9u) == 20000u,
               "the entries below shifted down one slot, the last dropped");

        /* the name grid: 0..0x19 = A..Z, 0x1A = space, 0x1B = '.', 0x1C =
         * delete, 0x1D = enter; 13 characters at most */
        nameBuf[0] = 0;
        name[0] = 0;
        _check(hiscore_name_apply(name, 0u, &len) == 0 && len == 1u &&
                   name[0] == 'A',
               "column 0 types 'A'");
        hiscore_name_apply(name, 26u, &len); /* space */
        hiscore_name_apply(name, 27u, &len); /* '.'   */
        _check(strcmp(name, "A .") == 0, "0x1A is space and 0x1B is '.'");
        hiscore_name_apply(name, 28u, &len);
        _check(strcmp(name, "A ") == 0, "0x1C deletes the last character");
        for (uint32_t i = 0; i < 20u; i++) hiscore_name_apply(name, 1u, &len);
        _check(len == 13u, "the buffer holds 13 characters");
        {
            char kept = name[11];

            hiscore_name_apply(name, 0u, &len);
            _check(len == 13u && name[12] == 'A' && name[11] == kept,
                   "a full buffer overwrites the last glyph (1000:044F)");
        }
        len = 0;
        name[0] = 0;
        name[-1] = 0x5A;
        hiscore_name_apply(name, 28u, &len);
        _check(len == 0u && name[-1] == 0,
               "an empty delete writes 0x756 (1000:046C)");
        _check(hiscore_name_apply(name, 29u, &len) == 1, "0x1D enters");

        /* the save image: 0x06CC encrypts the live table and writes it; the
         * file decrypts back to the same entries through the involution */
        hiscore_reset();
        _check(hiscore_insert(77777u, "ROUND TRIP") == 3,
               "77777 lands below 80000");
        _check(hiscore_score(3u) == 77777u &&
                   strcmp(hiscore_name(3u), "ROUND TRIP") == 0,
               "the insert rewrites the live entry");
        hiscore_write();
        {
            FSSave *f = g_osys_null.fs->openNamed("HISCORES.HUM");
            uint8_t raw[520];
            uint32_t n = 0;

            if (f) {
                n = g_osys_null.fs->saveRead(f, 0, raw, sizeof raw);
                g_osys_null.fs->closeSave(f);
            }
            if (n == sizeof raw) hiscore_obfuscate(raw, n);
            _check(n == sizeof raw &&
                       rd_le_u32(raw + 0x154u + 3u * 18u) == 77777u &&
                       strcmp((const char *)raw + 0x154u + 3u * 18u + 4u,
                              "ROUND TRIP") == 0,
                   "HISCORES.HUM holds the saved entry through the involution");
        }
        hiscore_reset();
        _check(hiscore_score(3u) == 70000u, "the reset restores the defaults");

        /* the commit's 0x1A/0x1B mapping reaches the stored name */
        hiscore_reset();
        hiscore_insert(200000u, "A\x1A"
                                "B\x1B"
                                "C");
        _check(strcmp(hiscore_name(0u), "A B.C") == 0,
               "0x1A/0x1B are stored as space and '.'");
        hiscore_reset();
    }

    _check(score_begin(0u) == 1, "the score screen starts");
    _flow_dump_ppm("score_port.ppm", osys_null_page(0));
    /* check_highscore (1000:41ae) writes CRTC 0x12 = 0x8F: the level's
     * mode-X field goes back to the 320x200 one */
    _check(osys_null_video_mode() == VIDEO_MODE_13H,
           "the score screen switches the display back to mode 13h");
    /* 1000:428D sets CRTC 0x12 = 0x8F (400 scanlines = the 320x200 field) at
     * the screen's start, so the entry mode is the card's; 0x439F's 0x8300 is
     * the full mode set at the end. */
    /* 1000:344C/0x3450: draw_level_text re-renders the world page after the
     * caption's key wait, so the caption ("YOUR TRIBE IS EXTINCT!!") is NOT
     * behind the card.  level_redraw_page must replace a sentinel page. */
    {
        uint8_t *pg = (uint8_t *)osys_null_page(0);
        uint32_t changed = 0;

        memset(pg, 0x5A, 320u * 200u);
        level_redraw_page();
        for (uint32_t i = 0; i < 320u * 200u; i++)
            if (pg[i] != 0x5A) changed++;
        _check(changed > 1000u,
               "1000:344C: the end page re-renders the world over the caption");
    }
    /* 1000:4403: the card's non-zero bytes are drawn as 0xC0 + v and a zero
     * byte is LEFT ALONE - the page keeps what the level caption left there
     * (the live stamp cell shows 1..0xBF inside the card where the sprite is
     * transparent).  Draw onto a sentinel page and _check the law directly. */
    {
        static uint8_t page[VIDEO_PAGE_BYTES];
        uint32_t kept = 0, drawn = 0, other = 0;

        memset(page, 0x5A, sizeof page);
        score_draw_card(page);
        for (uint32_t i = 0; i < sizeof page; i++) {
            if (page[i] == 0x5A) kept++;
            else if (page[i] >= 0xC0u) drawn++;
            else other++;
        }
        _check(other == 0u && drawn != 0u && kept != 0u,
               "the card is drawn as 0xC0 + v and leaves zero bytes alone");
        /* chunky 1:1 copy: pixel (x,y) = SCORE.BIT[y*320+x] + 0xC0.  These
         * three bytes (0x28/0x04/0x22) are exactly where a plane-3 subsample
         * would give 0x29/0x04/0x04, so they pin the linear read. */
        _check(page[18u * 320u + 18u] == 0xE8u &&
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
        uint32_t _bad = 0, kept = 0;

        for (uint32_t i = 0; i < 256u; i++) {
            pal[i * 3u + 0] = (uint8_t)(i & 0x3Fu);
            pal[i * 3u + 1] = (uint8_t)((i * 3u) & 0x3Fu);
            pal[i * 3u + 2] = (uint8_t)((i * 5u) & 0x3Fu);
        }
        palette_seal_triples(pal, 0u, 0x80u);
        for (uint32_t i = 0; i < 0x80u; i++) {
            uint32_t want = (77u * (i & 0x3Fu) + 151u * ((i * 3u) & 0x3Fu) +
                             28u * ((i * 5u) & 0x3Fu)) >>
                            9;

            if (pal[i * 3u] != want || pal[i * 3u + 1u] != want ||
                pal[i * 3u + 2u] != want)
                _bad++;
        }
        _check(_bad == 0u, "1000:33E2: the first 0x80 triples seal to grey");
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
            static uint8_t page[VIDEO_PAGE_BYTES];
            static uint8_t page2[VIDEO_PAGE_BYTES];

            memset(page, 0, sizeof page);
            score_column(page, 3u, 0, 0, 1u);
            _check(page[0 * 320u + 0] == 0xC2u && page[0 * 320u + 15] == 0xC2u,
                   "1000:462D: digit 3's row 0 is all 0xC2");
            _check(page[2 * 320u + 0] == 0xC2u && page[2 * 320u + 4] == 0xE9u &&
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
            _check(memcmp(page, page2, sizeof page) == 0,
                   "a 5-digit column is zero-padded and 16 px per digit");
        }

        /* 1000:44BB draw_scorenum_three_passes: the elapsed and best times are
         * three SCORENUM digits at row 181, px 80..87 and px 240..247, every
         * byte stored as v | 0xC0 (0x450F: 8 rows x 2 page bytes, +0x58/row). */
        {
            static uint8_t a[VIDEO_PAGE_BYTES], b[VIDEO_PAGE_BYTES];
            uint32_t ina = 0, outside = 0, low = 0, diff = 0;

            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            score_draw_num3(a, 0x50u, 0x0123u, 0xB5u, 0u);
            score_draw_num3(b, 0x50u, 0x0456u, 0xB5u, 0u);
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
            _check(ina != 0u && outside == 0u && low == 0u,
                   "1000:44BB draws the time digits at row 181, px 104..127");
            /* the first digit of 0x0123 is slot 1; its row 0 is
             * 17 17 02 02 02 02 17 17 -> D7 D7 C2 C2 C2 C2 D7 D7 */
            _check(a[181u * 320u + 104u] == 0xD7u &&
                       a[181u * 320u + 105u] == 0xD7u &&
                       a[181u * 320u + 106u] == 0xC2u,
                   "1000:450F: the SCORENUM slot is a chunky 8x8 image");
            _check(diff != 0u, "a different time draws different digits");
            memset(a, 0, sizeof a);
            score_draw_num3(a, 0xF0u, 0x0123u, 0xB5u, 0x280u);
            {
                uint32_t hit = 0;

                for (uint32_t y = 181u; y < 189u; y++)
                    for (uint32_t x = 264u; x < 288u; x++)
                        if (a[y * 320u + x]) hit++;
                _check(hit != 0u,
                       "the best time sits at px 264 with BP = 0x280");
            }
        }

        /* 1000:475D's tail + [0x1DC8] = 0x15E: once both terms are spent the
         * screen runs its own countdown and then ends by itself. */
        {
            uint32_t ticks = 0;

            score_prepare();
            while (score_counters_run() && ticks < 100000u) ticks++;
            _check(ticks >= 0x15Eu + 0x50u && ticks < 100000u,
                   "1000:475D: the screen ends on its own 0x15E countdown");
        }

        /* 1000:46FC: the NEW RECORD stamp - apl\RECORD.SPR's 16x24 cell drawn
         * at row 176, byte column 32 (px 128..191), one value per 4-px group,
         * non-zero as 0xC0 + v.  The shipped file's values are only {0, 2, 3},
         * so every set pixel must be 0xC2 or 0xC3, and its first four rows are
         * empty. */
        {
            static uint8_t page[VIDEO_PAGE_BYTES];
            uint32_t set = 0, _bad = 0, top = 0, outside = 0;

            memset(page, 0, sizeof page);
            score_stamp_draw(page, 1);
            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x++) {
                    uint8_t v = page[y * 320u + x];

                    if (!v) continue;
                    if (x < 128u || x >= 192u || y < 176u || y >= 200u)
                        outside++;
                    else if (y < 178u) top++;
                    else if (v != 0xC2u && v != 0xC3u) _bad++;
                    else set++;
                }
            _check(
                set != 0u && _bad == 0u && top == 0u && outside == 0u,
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
                _check(profile == 0x00FE7FFCu,
                       "the stamp is the sprite's chunky 64x24 row profile");
                _check(page[180u * 320u + 128u + 32u] == 0xC2u &&
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
            score_stamp_draw(page, 1);
            {
                uint32_t kept = 0, _bad = 0;

                for (uint32_t y = 0; y < 24u; y++)
                    for (uint32_t x = 0; x < 64u; x++) {
                        if (page[y * 320u + x] == 0x11u) kept++;
                        else _bad++;
                    }
                _check(kept == 64u * 24u && _bad == 0u,
                       "1000:4697: the scratch is not the visible page");
            }
            score_stamp_draw(page, 0);
            {
                uint32_t kept = 0, other = 0;

                for (uint32_t y = 176u; y < 200u; y++)
                    for (uint32_t x = 128u; x < 192u; x++) {
                        if (page[y * 320u + x] == 0x5Au) kept++;
                        else other++;
                    }
                _check(kept == 64u * 24u && other == 0u,
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
            score_stamp_draw(page, 1);
            score_stamp_draw(page, 0);
            {
                uint32_t kept = 0, _bad = 0;

                for (uint32_t y = 176u; y < 200u; y++)
                    for (uint32_t x = 128u; x < 192u; x++) {
                        if (page[y * 320u + x] == (uint8_t)(0x10u + (x & 3u)))
                            kept++;
                        else _bad++;
                    }
                _check(kept == 64u * 24u && _bad == 0u,
                       "the restore keeps the cell's 1-px detail");
            }
        }

        /* 1000:4333: the screen fades in before it takes input */
        {
            uint32_t t;

            for (t = 0; t < 200u && palette_fade_active(); t++) palette_tick();
            _check(!palette_fade_active(), "the score's fade-in completes");
        }
        _check(score_tick(&k) == 1, "the score screen's first tick arms");
        _check(score_tick(&k) == 1, "the score screen waits on the card");
        {
            uint32_t guard = 0;

            while (score_tick(&k) && guard++ < 5000u) palette_tick();
            _check(guard < 5000u, "the score screen exits after the hold");
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
            level_request_end(1);
            {
                uint8_t wantPal[768];
                uint8_t *raw;
                uint32_t sz = 0;

                memcpy(wantPal, level_palette(), sizeof wantPal);
                _check(score_begin(0u) == 1, "the blink screen starts");
                /* D-130: 1000:4287 seals entries 0x80..0xDF to grey and
                 * 1000:428A copies the card's 64 entries over 0xC0..0xFF. */
                palette_seal_triples(wantPal, 0x80u, 0x60u);
                raw = res_load_chunk("LBM\\SCORE.BIT", &sz, NULL);
                _check(raw != NULL && sz >= 320u * 200u + 768u,
                       "the score card loads for the palette test");
                if (raw && sz >= 320u * 200u + 768u)
                    memcpy(wantPal + 0xC0u * 3u, raw + 320u * 200u, 192u);
                free(raw);
                _check(memcmp(level_palette(), wantPal, sizeof wantPal) == 0,
                       "1000:4287/0x428A: the score prep seals 0x80..0xDF and "
                       "writes the card's 0xC0..0xFF bank");
            }

            /* 1000:467A: with NEW RECORD set the counter toggles on the borrow
             * (`SUB 1` / `JNS`); score_begin just prepared a record, so every
             * post-first gap is exactly 26 calls (the return is the new phase,
             * so track the phase itself, not the non-zero returns). */
            {
                uint32_t flips = 0, last = 0, badGap = 0;
                uint8_t prev = score_blink_on();

                for (uint32_t i = 1; i <= 160u; i++) {
                    uint8_t now;

                    (void)score_blink_tick();
                    now = score_blink_on();
                    if (now == prev) continue;
                    flips++;
                    if (last != 0u && i - last != 26u) badGap = 1;
                    last = i;
                    prev = now;
                }
                _check(
                    flips >= 4u && badGap == 0u,
                    "1000:467A: the NEW RECORD blink toggles every 26 calls");
            }
            for (t = 0; t < 400u && palette_fade_active(); t++) {
                (void)score_tick(&tk);
                palette_tick();
            }
            (void)score_tick(&tk); /* the arm tick */
            for (t = 0; t < 400u && score_blink_on() == 0; t++) {
                (void)score_tick(&tk);
                palette_tick();
            }
            for (uint32_t r = 0; r < 24u; r++)
                for (uint32_t x = 0; x < 64u; x++)
                    cellOn[r * 64u + x] = pg[(176u + r) * 320u + 128u + x];
            for (t = 0; t < 400u && score_blink_on() == 1; t++) {
                (void)score_tick(&tk);
                palette_tick();
            }
            for (uint32_t r = 0; r < 24u; r++)
                for (uint32_t x = 0; x < 64u; x++)
                    cellOff[r * 64u + x] = pg[(176u + r) * 320u + 128u + x];
            diff = 0;
            for (uint32_t i = 0; i < 24u * 64u; i++)
                if (cellOn[i] != cellOff[i]) diff++;
            _check(diff != 0u,
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
                if (object_low_phase_match() < 0) {
                    met = 1;
                    break;
                }
            }
            _check(met, "a goal tile one row below counts (1000:3624)");
            /* negative control: the same cell cleared never meets the goal */
            map[cell + 32u] = 0x00u;
            met = 0;
            for (uint32_t i = 0; i < 0x8Cu + 8u; i++) {
                object_tick(pal);
                if (object_low_phase_match() < 0) {
                    met = 1;
                    break;
                }
            }
            _check(!met, "with neither cell the goal is not met");

            /* D-100: `[DI+0x20]` does not wrap to row 0.  With the record in
             * the map's last row (cell 608) the below-cell is the buffer's
             * 32-byte gap (0x00), so a 0xA8 planted in row 0 must not count. */
            map[608u] = 0x00u;
            map[0] = 0xA8u;
            r[0] = 0x00u;
            r[1] = 0x00u;
            r[2] = 0x24u;
            r[3] = 0x01u; /* y = 0x124 -> (y + 0xC)/16 = 19 */
            met = 0;
            for (uint32_t j = 0; j < 0x8Cu + 8u; j++) {
                object_tick(pal);
                if (object_low_phase_match() < 0) {
                    met = 1;
                    break;
                }
            }
            _check(!met, "D-100: the last row's below-cell is the gap");
            /* 1000:3149 copies the map again at +0x2A0.  The second copy is
             * the LOAD-TIME map (the loader writes it once), so re-seed it to
             * prove y = 0x144 reads that copy and not the live first 640. */
            r[2] = 0x44u;
            r[3] = 0x01u;
            {
                static uint8_t snap[640];

                memset(snap, 0, sizeof snap);
                snap[0] = 0x5Au;
                object_map_tail_reset(snap);
                _check(object_record_map_cell(0u) == 0x5Au,
                       "D-100: the map's second copy is read (1000:3149)");
                map[0] = 0x00u;
                object_map_tail_reset(map);
            }
        }
    }

    /* D-196: objectTest mode 0 is pred_type_low_phase_match (1000:3600), not
     * "no test"; level_end_test must run it.  Level 2 (ANDIE PANDY) has
     * objectTest 0 and its 0xA8 goal at cell 629 (col 21, row 19). */
    if (level_load(1) == 0) {
        uint8_t *r = object_slot(0u);
        int32_t ended = 0;
        uint32_t i;

        object_init();
        object_set_action_entity(0u);
        r[0] = 0x48u; /* 328 */
        r[1] = 0x01u;
        r[2] = 0x24u; /* 292 */
        r[3] = 0x01u;
        r[4] = 0x10u;
        r[6] = 0u;
        r[8] = 0x02u;
        r[9] = 0u;
        _check(level_map()[629] == 0xA8u,
               "level 2 carries the 0xA8 goal at cell 629");
        for (i = 0; i < 0x8Cu + 8u; i++) {
            object_tick(level_palette());
            if (level_end_test()) {
                ended = 1;
                break;
            }
        }
        _check(ended && level_outcome(),
               "mode 0's 0xA8 goal ends level 2 (D-196)");

        /* negative control: the same frames off the 0xA8 tile do not end it */
        if (level_load(1) == 0) {
            ended = 0;
            object_init();
            object_set_action_entity(0u);
            r = object_slot(0u);
            r[0] = 0x40u;
            r[1] = 0u;
            r[2] = 0x40u;
            r[3] = 0u;
            r[4] = 0x10u;
            r[6] = 0u;
            r[8] = 0x02u;
            r[9] = 0u;
            for (i = 0; i < 0x8Cu + 8u; i++) {
                object_tick(level_palette());
                if (level_end_test()) {
                    ended = 1;
                    break;
                }
            }
            _check(!ended, "off the 0xA8 tile level 2 does not end");
        }
    }

    /* 7a-bis. 1000:F423's rect is the **TER slot**, not the type row: 1000:F3CA
     * leaves DI = 0x649A and 1000:F3ED calls F423 twice (DI, then DI+0x14), so
     * the actor is tested against the two 0x649A/0x64AE boxes.  A match ORs the
     * type's 1<<(7-(type&7)) bit into the slot's +0x13 carry byte and refuses
     * the settle; outside the box (or disabled) the bit is cleared and the
     * settle runs. */
    {
        if (level_load(4) == 0) {
            uint8_t *s0 = object_ter_slot_at(0u);
            uint8_t *a = object_slot(2u); /* record 2: the live rider's slot */

            memset(a, 0, 0x12u);
            _tf_wr16(s0, 0u, 200u);
            _tf_wr16(s0, 2u, 200u);
            s0[0x10] = 0;
            s0[0x11] = 0; /* enabled */
            s0[0x12] = 0;
            s0[0x13] = 0; /* mask clear */
            _tf_wr16(a, 0u, 192u); /* slot.x - 8 */
            _tf_wr16(a, 2u, 193u); /* slot.y - 7 */
            _tf_wr16(a, 8u, 2u); /* type 2 -> bit 0x20 = record 2 */
            _check(object_settle_gate_for_probe(a) == 0 && s0[0x13] == 0x20u,
                   "1000:F423: the TER box sets the type's carry bit");
            {
                uint16_t x0 = rd_le_u16(s0 + 0u), y0 = rd_le_u16(s0 + 2u);

                object_camera_slots_tick();
                _check(rd_le_u16(a + 0u) ==
                               (uint16_t)(192u + (rd_le_u16(s0 + 0u) - x0)) &&
                           rd_le_u16(a + 2u) ==
                               (uint16_t)(193u + (rd_le_u16(s0 + 2u) - y0)),
                       "D-227: record 2 in the box rides the slot");
            }
            _tf_wr16(a, 0u, 230u); /* out of the x window */
            _check(object_settle_gate_for_probe(a) == 1 && s0[0x13] == 0u,
                   "1000:F423: outside the TER box the bit is cleared");
            /* D-227: the original stores the TYPE bit (all humans type 2 -> the
             * mask always carries record 2); the port stores the ACTING
             * record's index, so record 0/type 2 sets 0x80 and the next gate
             * pass clears the same bit. */
            a = object_slot(0u);
            memset(a, 0, 0x12u);
            _tf_wr16(a, 0u, 192u);
            _tf_wr16(a, 2u, 193u);
            _tf_wr16(a, 8u, 2u);
            s0[0x13] = 0u;
            _check(object_settle_gate_for_probe(a) == 0 && s0[0x13] == 0x80u,
                   "D-227: record 0/type 2 marks record 0, not record 2");
            {
                uint16_t x0 = rd_le_u16(s0 + 0u), y0 = rd_le_u16(s0 + 2u);

                object_camera_slots_tick();
                _check(rd_le_u16(a + 0u) ==
                               (uint16_t)(192u + (rd_le_u16(s0 + 0u) - x0)) &&
                           rd_le_u16(a + 2u) ==
                               (uint16_t)(193u + (rd_le_u16(s0 + 2u) - y0)),
                       "D-227: record 0 (not record 2) rides the slot");
            }
            _tf_wr16(a, 0u, 230u);
            _check(object_settle_gate_for_probe(a) == 1 && s0[0x13] == 0u,
                   "D-227: the same record is cleared when it leaves the box");
            s0[0x13] = 0u;
            _tf_wr16(a, 0u, 192u);
            wr_le_u16(s0 + 0x10u, 0xFFFFu); /* disabled */
            _check(object_settle_gate_for_probe(a) == 1 && s0[0x13] == 0u,
                   "1000:F423: a disabled TER slot never claims");
        }
    }

    /* 7a-ter. D-228: 1000:C444's hit path rewrites the matched HUMAN slot, not
     * the marker record.  The marker's +8 (its map cell) and +4 (0x26) must
     * survive so 1000:C541 keeps animating the right cell. */
    {
        static uint8_t map[640];
        uint8_t *h, *m;

        object_init();
        memset(map, 0, sizeof map);
        map[5u * 32u + 5u] = 0xBBu;
        _check(object_load_terrain_objects(map) == 9u,
               "D-228: the 0xBB tile appends the marker record");
        m = object_slot(9u);
        h = object_slot(0u);
        _check(m && h && rd_le_u16(m + 8u) == 5u * 32u + 5u,
               "D-228: the marker points at its map cell");
        memset(h, 0, 0x12u);
        _tf_wr16(h, 0u, 80u); /* marker.x */
        _tf_wr16(h, 2u, 72u); /* same 16-px tile row as the marker */
        _tf_wr16(h, 4u, 0x0010u);
        _tf_wr16(h, 8u, 2u);
        object_update_list();
        _check(
            rd_le_u16(h + 0u) == 72u && rd_le_u16(h + 4u) == 0x0028u &&
                rd_le_u16(h + 8u) == 0x0016u && rd_le_u16(h + 0xAu) == 5u,
            "D-228: the hit rewrites the human (x-8, frame 0x28, type 0x16)");
        _check(rd_le_u16(m + 8u) == 5u * 32u + 5u &&
                   rd_le_u16(m + 4u) == 0x0026u,
               "D-228: the marker keeps its map cell and frame");
    }

    /* 7a-quater. 1000:3540's goal pan is a blocking loop: each nested
     * update_world calls 1000:E3F6 -> 1000:8526 poll_input, so a released
     * direction stops the human, and the pan does not re-check [0x6530] once
     * it has started. */
    if (level_load(6) == 0) {
        InputState in;
        uint8_t *h = object_slot(0u);

        level_begin();
        for (uint32_t t = 0; t < 4000u && level_starting(); t++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        object_set_slot_class(1u,
                              9u); /* record 9 is the pan's class-9 target */
        object_set_action_entity(0u);
        memset(h, 0, 0x12u);
        _tf_wr16(h, 0u, 448u);
        _tf_wr16(h, 2u, 264u);
        _tf_wr16(h, 4u, 0x0010u);
        _tf_wr16(h, 8u, 2u);
        object_goal_scan();
        _check(object_goal_flag(), "the human at level 6's 0xBA sets the goal");
        memset(&in, 0, sizeof in);
        in.joyBits = 0x08u; /* right */
        level_note_input(&in);
        level_tick();
        _check(level_goal_pan_running_for_probe() != 0,
               "1000:3540 starts the goal pan");
        {
            uint16_t x0 = rd_le_u16(h + 0u);

            memset(&in, 0, sizeof in);
            level_note_input(&in);
            for (uint32_t k = 0; k < 8u; k++) level_tick();
            _check(rd_le_u16(h + 0u) == x0,
                   "the goal pan re-polls and the released human stops");
        }
        /* let the pan finish so _goal_follow_record is cleared before the
         * camera-follower tests below */
        for (uint32_t k = 0; k < 2000u && level_goal_pan_running_for_probe();
             k++) {
            memset(&in, 0, sizeof in);
            level_note_input(&in);
            level_tick();
        }
        _check(!level_goal_pan_running_for_probe(), "the goal pan completes");
    }

    /* 7b. the rest of the type rows' handlers (DS:0x6918) */
    {
        uint8_t *a = object_slot(0u);

        /* type 18 (0x12), input 0x06 -> normalised 0x0A -> key 0x05 -> command
         * 5 -> index 37 (1000:EDB7): the home-state transition */
        wr_le_u16((a) + (8u), 0x12u);
        wr_le_u16((a) + (4u), 0x20u);
        wr_le_u16((a) + (0xAu), 0x7u);
        wr_le_u16((a) + (0xCu), 0x7u);
        wr_le_u16((a) + (0xEu), 0x7u);
        wr_le_u16((a) + (0x10u), 0x7u);
        _check(object_command_run(a, 0x06u) == OBJ_CMD_HANDLER,
               "type 18 command 5 runs the home-state handler");
        _check(rd_le_u16((a) + (4u)) == 0u && rd_le_u16((a) + (8u)) == 0u,
               "type 0x12 becomes frame 0 / type 0 (1000:EDD9)");
        _check(rd_le_u16((a) + (0xAu)) == 0u && rd_le_u16((a) + (0xEu)) == 0u &&
                   rd_le_u16((a) + (0x10u)) == 0u,
               "the pending words are cleared");
        _check(object_action_latched(),
               "the transition consumes the ACTION press");

        /* input 0x81 -> half 1, command 0 -> index 38 (1000:EE07): the turn,
         * which needs the ACTION press to have been released (1000:EE0C) */
        object_set_action_latched(0);
        wr_le_u16((a) + (8u), 0x12u);
        wr_le_u16((a) + (0xEu), 0u);
        _check(object_command_run(a, 0x81u) != OBJ_CMD_UNPORTED,
               "type 18 half-1 command 0 runs the turn handler");
        _check(rd_le_u16((a) + (0xEu)) == 4u,
               "the turn advances the direction counter by 4");

        /* input 0x01 -> half 0, command 0 -> index 39 (1000:EE3B): the door */
        a[7] = 0u; /* class 0: 1000:EEB8's `DEC DX`/`MUL DL` lands at 0x5105 */
        wr_le_u16((a) + (4u), 0x8Cu);
        _check(object_command_run(a, 0x01u) != OBJ_CMD_UNPORTED,
               "the latched latch runs the enter-the-building sequence");
        _check(rd_le_u16((a) + (4u)) == 0u && rd_le_u16((a) + (8u)) == 0u,
               "the door vector 0 sets frame 0 / type 0");
        _check(object_last_sfx() == 3u, "the entry plays effect 3 (1000:EEF2)");
        _check(!object_action_latched(), "the entry releases the latch");
        _check(object_ds_tail5105(8u) == 0xFFu &&
                   object_ds_tail5105(9u) == 0xFFu &&
                   object_ds_tail5105(10u) == 0u &&
                   object_ds_tail5105(11u) == 0u &&
                   object_ds_tail5105(12u) == 3u &&
                   object_ds_tail5105(13u) == 0u &&
                   object_ds_tail5105(14u) == 0xFBu &&
                   object_ds_tail5105(15u) == 0xFFu,
               "the class-0 door write lands at 0x510D..0x5114 (D-24)");

        /* the `0x6E76` door table's rows 1/2 (frames 0x8D/0x8E): the image's
         * y words are -7 and -9.  The class byte is 1, so the placed record is
         * slot 8 + cls = 9 and its class byte is [0x3E5E + 1]. */
        {
            uint8_t *placed = object_slot(9u);
            static const int16_t doorY[2] = { -7, -9 };

            for (uint32_t k = 0; k < 2u; k++) {
                object_set_slot_class_for_probe(1u, 0xFFu);
                memset(placed, 0, 0x12u);
                wr_le_u16((a) + (8u), 0x12u);
                wr_le_u16((a) + (4u), 0x8Cu);
                wr_le_u16((a) + (0xEu), 0u);
                a[7] = 1u;
                /* the turn re-arms [0x6F1D] */
                (void)object_command_run(a, 0x81u);
                wr_le_u16((a) + (4u), (uint16_t)(0x8Du + k));
                (void)object_command_run(a, 0x01u);
                _check((int16_t)rd_le_u16((placed) + (0xEu)) == doorY[k] &&
                           rd_le_u16((placed) + (0xCu)) == (uint32_t)(2u - k),
                       "the door table's row matches 0x6E76");
                _check(object_slot_class(1u) == 0x3Fu && placed[8] == 0xFFu,
                       "the door masks the class byte and marks slot 8+cls");
            }
        }

        /* the type-17 9-beat (1000:ECB7): a live beat returns at 1000:ECBB's
         * JNS without the restore; the expiry runs it. */
        wr_le_u16((a) + (8u), 0x11u);
        wr_le_u16((a) + (4u), 0x30u);
        wr_le_u16((a) + (0xAu), 5u);
        wr_le_u16((a) + (0xCu), 0u);
        wr_le_u16((a) + (0xEu), 0x111u);
        wr_le_u16((a) + (0x10u), 0x222u);
        (void)object_command_run(a, 0u);
        _check(rd_le_u16((a) + (0xAu)) == 4u &&
                   rd_le_u16((a) + (4u)) == 0x30u &&
                   rd_le_u16((a) + (8u)) == 0x11u,
               "a live 9-beat keeps the frame and type (1000:ECBB)");
        wr_le_u16((a) + (0xAu), 0u);
        (void)object_command_run(a, 0u);
        _check(rd_le_u16((a) + (4u)) == 0x111u &&
                   rd_le_u16((a) + (8u)) == 0x222u &&
                   rd_le_u16((a) + (0xAu)) == 0u &&
                   rd_le_u16((a) + (0xCu)) == 0u,
               "the expiry restores frame/type from +0xE/+0x10 (1000:ECDD)");

        /* type 18 command 6 -> index 33 (1000:ED4F): the frame nudge toward
         * 0x8E, one step per 25 frames */
        wr_le_u16((a) + (8u), 0x12u);
        wr_le_u16((a) + (4u), 0x8Cu);
        wr_le_u16((a) + (0xAu), 0u);
        wr_le_u16((a) + (0xEu), 0x19u);
        {
            static uint8_t pal[768];

            /* 1000:F9E4 counts one tick per *frame* ([0x4103]), so the frame
             * has to advance between the calls: object_tick is what counts it. */
            for (uint32_t i = 0; i < 28u; i++) {
                (void)object_command_run(a, 0x04u); /* normalises to key 8 */
                object_tick(pal);
            }
        }
        _check(rd_le_u16((a) + (4u)) == 0x8Du,
               "the 25-frame cadence steps the frame once");

        /* 1000:D9E8 (type 3's tail): the two deterministic branches.
         *   - y + 0x18 >= 0x158 takes the hard landing (0xDB15);
         *   - a 0xF0 cell one row above the probe is the platform snap. */
        {
            uint8_t *m = (uint8_t *)level_map();

            wr_le_u16((a) + (8u), 3u);
            wr_le_u16((a) + (4u), 0x20u);
            wr_le_u16((a) + (0u), 100u);
            wr_le_u16((a) + (2u), 0x150u); /* probe y 0x168 >= 0x158 */
            wr_le_u16((a) + (0xAu), 0u);
            (void)object_command_run(a, 0x01u);
            _check(rd_le_u16((a) + (0u)) == 93u &&
                       rd_le_u16((a) + (4u)) == 0x28u &&
                       rd_le_u16((a) + (8u)) == 0x16u &&
                       rd_le_u16((a) + (0xAu)) == 5u && object_last_sfx() == 4u,
                   "a probe past 0x158 takes the hard landing (1000:DB15)");
            wr_le_u16((a) + (8u), 3u); /* the crash left type 0x16 */
            wr_le_u16((a) + (0u), 100u);
            wr_le_u16((a) + (2u), 100u);
            wr_le_u16((a) + (4u), 0x20u);
            wr_le_u16((a) + (0xAu), 0u);
            if (m) m[198] = 0xF0u; /* 2*0x60 + (108 >> 4) */
            (void)object_command_run(a, 0x01u);
            /* 1000:DA3E..0xDA61 writes only y (rowY - 0x1C); the x is left
             * alone - live level 16 drowns at (344,292), stable (D-78). */
            _check(
                rd_le_u16((a) + (0u)) == 100u && rd_le_u16((a) + (2u)) == 84u &&
                    rd_le_u16((a) + (4u)) == 0xCDu &&
                    rd_le_u16((a) + (8u)) == 0x1Cu &&
                    rd_le_u16((a) + (0xAu)) == 9u && object_last_sfx() == 6u,
                "a 0xF0 cell one row above is the platform snap (1000:DA3E)");
            if (m) m[198] = 0u;
        }

        /* the path handlers (indices 25 / 27): a type-0x0F record whose +0xE
         * links to another record (the 1000:9CF0 give/hold form), whose +8 is
         * the progress.  1000:EBA0 nudges it through 1000:FC32. */
        {
            uint8_t *node = object_slot(1u);

            wr_le_u16((a) + (8u), 0x0Fu);
            wr_le_u16((a) + (4u), 0x20u);
            wr_le_u16((a) + (0xAu), 0u);
            wr_le_u16((a) + (0xEu), 1u * 0x12u + 4u); /* the link to slot 1 */
            memset(node, 0, 0x12u);
            /* the handler and the type-15 rows' *tail* (0xDC1F,
             * `objCmdType15Tail`) both run; the checks below read the state
             * they leave. */
            (void)object_command_run(a, 0x08u);
            _check(rd_le_u16((node) + (8u)) == 1u &&
                       rd_le_u16((a) + (0xAu)) == 1u,
                   "the nudge adds the actor's ramped +0xA to the node's +8");
            for (uint32_t i = 0; i < 4u; i++)
                (void)object_command_run(a, 0x08u);
            _check(rd_le_u16((node) + (8u)) == 10u,
                   "the progress ramps 1+2+3+4 and stops at 0xA (1000:FC78)");
            (void)object_command_run(a, 0x08u);
            _check(rd_le_u16((node) + (8u)) == 10u, "and stays there");
            /* the park: [node+7] & 7 == 4 refuses all further nudges */
            node[7] = 0x0Cu;
            (void)object_command_run(a, 0x08u);
            _check(rd_le_u16((node) + (8u)) == 10u,
                   "a parked node is not nudged");
            /* the arrival: a progress already past -0xF takes frame 0x4A */
            node[7] = 0u;
            wr_le_u16((node) + (8u), 0xFFF1u); /* -15 */
            (void)object_command_run(a, 0x08u);
            _check(rd_le_u16((a) + (4u)) == 0x4Au && node[7] == 0x0Cu,
                   "reaching +-0xF sets frame 0x4A and parks the node");
            /* 1000:EBD0 (type 15 half-1 command 1): the advance writes the
             * node's y at +0x10 and its scaled step at +0xA (the listing's
             * `ADD DI,2` at 1000:EC45), leaving the progress word at +8 */
            node[7] = 0u;
            memset(node, 0, 0x12u);
            wr_le_u16((node) + (0u), 100u);
            wr_le_u16((node) + (2u), 100u);
            wr_le_u16((node) + (8u), 5u); /* a live progress word */
            (void)object_command_run(a, 0x89u);
            _check(rd_le_u16((node) + (0x10u)) == 100u,
                   "the advance stores the node's y at +0x10");
            _check(
                rd_le_u16((node) + (8u)) == 5u &&
                    rd_le_u16((node) + (0xAu)) != 0u,
                "the advance stores its step at +0xA, not in +8 (1000:EC78)");

            /* D-110: the EC27 sign is field A (the slope), not field B (the
             * item record).  Find a tile/sub where the two signs differ,
             * place it under the node and _check the step the slope selects. */
            {
                uint8_t *map = (uint8_t *)level_map();
                uint32_t t, sub, tile = 0x100u, x = 0, cell = 0;
                int32_t slope = 0;

                for (t = 0; t < 256u && tile == 0x100u; t++)
                    for (sub = 0; sub < 16u; sub++) {
                        int32_t s = level_slope_byte(t, sub);

                        if ((uint32_t)(s & 0x80) !=
                            (level_item_byte(t, sub) & 0x80u)) {
                            tile = t;
                            x = 100u + ((sub + 16u - 12u) & 0xFu);
                            cell = 7u * 32u + (x >> 4);
                            slope = s;
                            break;
                        }
                    }
                _check(map != NULL && tile < 0x100u,
                       "the D-110 slope/item sign pair exists");
                if (map && tile < 0x100u) {
                    uint8_t old = map[cell];
                    uint32_t want = ((slope & 0x80) ? 1u : 3u) + 3u;

                    map[cell] = (uint8_t)tile;
                    node[7] = 0u;
                    memset(node, 0, 0x12u);
                    wr_le_u16((node) + (0u), x);
                    wr_le_u16((node) + (2u), 100u);
                    wr_le_u16((node) + (8u), 0u);
                    (void)object_command_run(a, 0x89u);
                    _check(rd_le_u16((node) + (0xAu)) == want,
                           "the path advance tests the slope sign (D-110)");
                    map[cell] = old;
                }
            }
        }

        /* the type-12 tail's three-way choice (1000:EAE6): the span
         * end `[node+4]+[node+6]` vs the record's y, then the page byte and
         * the surface line vs the probe line `y + 0x18`. */
        {
            uint8_t *span = object_span_at_for_probe(0u);
            uint8_t *m = (uint8_t *)level_map();
            uint32_t solid = 0x100u, open = 0x100u, old = 0u, t;

            for (t = 0; t < 256u; t++) {
                uint32_t b = level_page_byte(t, 12u); /* sub = 108 & 0xF */
                if ((b & 0x80u) && solid == 0x100u) solid = t;
                if (!(b & 0x80u) && open == 0x100u) open = t;
            }
            _check(m != NULL && solid < 0x100u && open < 0x100u,
                   "the type-12 probe tiles are in the page table");
            if (m && solid < 0x100u && open < 0x100u) {
                memset(span, 0, 0x1Cu);
                wr_le_u16((a) + (0u), 100u);
                wr_le_u16((a) + (0xEu),
                          0u); /* the type-12 tail reads span entry 0 */
                wr_le_u16((a) + (0xAu),
                          4u); /* the step countdown does not expire */
                old = m[102]; /* row 3 (y = 0x38), col 6 */
                m[102] = (uint8_t)solid;
                /* (a) the span end 0x10 is above the record's y 0x30: home,
                 * even though the cell under the probe is solid */
                wr_le_u16((a) + (2u), 0x30u);
                wr_le_u16((a) + (4u), 0x20u);
                wr_le_u16((a) + (8u), 0x0Cu);
                wr_le_u16((span) + (4u), 0x10u);
                wr_le_u16((span) + (6u), 0u);
                (void)object_command_run(a, 0u);
                _check(
                    rd_le_u16((a) + (2u)) == 0x30u &&
                        rd_le_u16((a) + (4u)) == 0x10u &&
                        rd_le_u16((a) + (8u)) == 2u,
                    "type 12: a span end above the record is home (1000:EAF9)");
                /* (b) below the record, a solid page byte: the stepper */
                wr_le_u16((a) + (2u), 0x20u);
                wr_le_u16((a) + (4u), 0x20u);
                wr_le_u16((a) + (8u), 0x0Cu);
                wr_le_u16((span) + (4u), 0x30u);
                wr_le_u16((span) + (6u), 0x20u);
                (void)object_command_run(a, 0u);
                _check(
                    rd_le_u16((a) + (2u)) == 0x21u &&
                        rd_le_u16((a) + (4u)) == 0x20u &&
                        rd_le_u16((a) + (8u)) == 0x0Cu,
                    "type 12: a solid cell takes the down-stepper (1000:EB4B)");
                /* (c) below the record, the surface line inside the 0x18 band:
                 * home against the probe line, not against the record's y */
                wr_le_u16((a) + (2u), 0x20u);
                wr_le_u16((a) + (4u), 0x20u);
                wr_le_u16((a) + (8u), 0x0Cu);
                m[102] = (uint8_t)open;
                (void)object_command_run(a, 0u);
                _check(rd_le_u16((a) + (2u)) == 0x20u &&
                           rd_le_u16((a) + (4u)) == 0x10u &&
                           rd_le_u16((a) + (8u)) == 2u,
                       "type 12: the surface is compared with the probe line");
                m[102] = (uint8_t)old;
            }
        }

        /* D-117: type 11 reads the span's word 0 as the owner record. */
        {
            uint8_t *span = object_span_at_for_probe(0u);
            uint8_t *owner = object_slot(3u);

            memset(span, 0, 0x1Cu);
            wr_le_u16((span) + (0u), 3u); /* the span's owner record */
            memset(owner, 0, 0x12u);
            wr_le_u16((owner) + (2u), 200u);
            wr_le_u16((owner) + (8u), 9u);
            wr_le_u16((a) + (0u), 100u);
            wr_le_u16((a) + (2u), 100u);
            wr_le_u16((a) + (4u), 0x20u);
            wr_le_u16((a) + (8u), 0x0Bu); /* type 11 */
            wr_le_u16((a) + (0xEu), 0u); /* span entry 0 */
            (void)object_command_run(a, 0u);
            _check(rd_le_u16((a) + (4u)) == 8u &&
                       rd_le_u16((a) + (8u)) == 0x0Du &&
                       rd_le_u16((a) + (0xEu)) == 0x0Au,
                   "type 11 homing uses the span owner record (D-117)");
        }

        /* D-150: a solid neighbour leaves cellOut on the point's tile. */
        {
            uint8_t *m = (uint8_t *)level_map();
            uint32_t solidA = 0x100u, solidB = 0x100u, zero = 0x100u;
            uint32_t got = 0xFFFFu, i;
            uint8_t oldPoint = 0, oldAbove = 0, oldBelow = 0;
            int32_t r;

            for (i = 0; i < 256u; i++) {
                uint32_t b = level_page_byte(i, 4u);

                if (zero == 0x100u && b == 0u) zero = i;
                if (b & 0x80u) {
                    if (solidA == 0x100u) solidA = i;
                    else if (i != solidA) solidB = i;
                }
            }
            _check(m != NULL && solidB < 0x100u && zero < 0x100u,
                   "the D-150 tiles exist");
            if (m && solidB < 0x100u) {
                oldPoint = m[102];
                oldAbove = m[70];
                oldBelow = m[134];
                m[102] = (uint8_t)zero;
                m[70] = (uint8_t)solidA;
                r = object_land_y_for_probe(100, 0x30, &got);
                _check(r == 0x30 && got == zero,
                       "a solid row above keeps the point tile (D-150)");
                m[102] = (uint8_t)solidA;
                m[134] = (uint8_t)solidB;
                r = object_land_y_for_probe(100, 0x30, &got);
                _check(r == -1 && got == solidA,
                       "a solid row below keeps the point tile (D-150)");
                m[102] = oldPoint;
                m[70] = oldAbove;
                m[134] = oldBelow;
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

            wr_le_u16((a) + (8u), 0x0Fu);
            wr_le_u16((a) + (4u), 0x20u);
            wr_le_u16((a) + (0xEu), 1u * 0x12u + 4u); /* the link to slot 1 */
            memset(node, 0, 0x12u);
            wr_le_u16((node) + (0u), 100u);
            wr_le_u16((node) + (2u), 100u);
            wr_le_u16((node) + (8u), 1u); /* a non-zero progress */
            memset(target, 0, 0x12u);
            wr_le_u16((target) + (0u), 108u); /* node.x + 8 */
            wr_le_u16((target) + (2u), 92u); /* node.y - 8 */
            wr_le_u16((target) + (8u), 1u); /* type 1 */
            _check(
                object_command_run(a, 0x01u) == OBJ_CMD_TAIL,
                "a type-15 command with no handler reports its tail (1000:DC1F)");
            _check(rd_le_u16((target) + (4u)) == 0x55u &&
                       rd_le_u16((target) + (8u)) == 0x10u,
                   "the record in the window takes the carried form");
            _check(rd_le_u16((target) + (0xAu)) == 0u &&
                       rd_le_u16((target) + (0xCu)) == 0u,
                   "the carried form clears +0xA and +0xC");
            /* a word add, so the phase (0..0x3F) can carry past 0xFF */
            _check(rd_le_u16((target) + (0xEu)) >= 0xF0u &&
                       rd_le_u16((target) + (0xEu)) <= 0x12Fu,
                   "the carried form's +0xE is 0xF0 + the frame phase");
            _check(object_last_sfx() == 8u, "the carried form plays effect 8");
            memset(target, 0, 0x12u);
            memset(node, 0, 0x12u);
        }

        /* the same tail through the 0x6F22 group pass: the entry's first
         * member (e[5]) names the placed record, and the carry form must land
         * on that record (1000:DD2D's PUSH DI / CALL 0xFA13 / POP BP), not on
         * the entry. */
        {
            uint8_t *node = object_slot(1u);
            uint8_t *target = object_slot(4u);
            uint8_t *grp = object_group_at(0u);

            wr_le_u16((a) + (8u), 0x0Fu);
            wr_le_u16((a) + (0xEu), 1u * 0x12u + 4u); /* the link to slot 1 */
            memset(node, 0, 0x12u);
            wr_le_u16((node) + (0u), 100u);
            wr_le_u16((node) + (2u), 100u);
            wr_le_u16((node) + (8u), 1u);
            memset(target, 0, 0x12u);
            wr_le_u16((target) + (0u), 108u);
            wr_le_u16((target) + (2u), 92u);
            wr_le_u16((target) + (8u), 1u);
            memset(grp, 0, 0x0Eu);
            wr_le_u16((grp) + (0u), 108u);
            wr_le_u16((grp) + (2u), 92u);
            grp[5] = 4u; /* the placed record */
            grp[6] = 0x80u; /* the member-list terminator */
            _check(object_command_run(a, 0x01u) == OBJ_CMD_TAIL,
                   "the group pass reaches the type-15 tail");
            _check(rd_le_u16((target) + (4u)) == 0x55u &&
                       rd_le_u16((target) + (8u)) == 0x10u,
                   "the group carry writes the placed record (D-158)");
            memset(target, 0, 0x12u);
            memset(node, 0, 0x12u);
            memset(grp, 0, 0x0Eu);
        }

        /* D-153/D-93: the dissolve clears +0/+2 and the five list words; the
         * list is [owner/base, ..., top], so FADA resets the top and FAA4
         * stands the FIRST member (the owner/base).  D-231. */
        {
            uint8_t *e = object_group_at(0u);
            uint8_t *owner = object_slot(4u), *top = object_slot(5u);

            memset(e, 0, 0x0Eu);
            wr_le_u16((e) + (2u), 0x1234u);
            e[5] = 4u;
            e[6] = 5u;
            e[7] = 0x80u;
            memset(owner, 0, 0x12u);
            wr_le_u16((owner) + (2u), 100u);
            wr_le_u16((owner) + (4u), 0x60u);
            wr_le_u16((owner) + (8u), 3u);
            memset(top, 0, 0x12u);
            wr_le_u16((top) + (2u), 84u);
            wr_le_u16((top) + (4u), 0x60u);
            wr_le_u16((top) + (8u), 3u);
            object_group_dissolve_at(0u);
            _check((e[1] & 0x80u) && rd_le_u16((e) + (2u)) == 0xFFFFu &&
                       rd_le_u16((e) + (4u)) == 0xFFFFu &&
                       rd_le_u16((e) + (6u)) == 0xFFFFu &&
                       rd_le_u16((e) + (8u)) == 0xFFFFu &&
                       rd_le_u16((e) + (0xAu)) == 0xFFFFu &&
                       rd_le_u16((e) + (0xCu)) == 0xFFFFu,
                   "the group dissolve clears +0/+2 and +4..+0xD (D-153)");
            _check(rd_le_u16((owner) + (2u)) == 92u &&
                       rd_le_u16((owner) + (4u)) == 0x10u &&
                       rd_le_u16((owner) + (8u)) == 2u,
                   "the owner/base takes the FAA4 tail (D-93, D-231)");
            _check(rd_le_u16((top) + (4u)) == 0x10u &&
                       rd_le_u16((top) + (8u)) == 3u,
                   "the top member takes the FADA reset (D-231)");
            /* three members: reset the top and the middle, stand the first */
            {
                uint8_t *mid = object_slot(6u);

                memset(e, 0, 0x0Eu);
                e[5] = 4u;
                e[6] = 5u;
                e[7] = 6u;
                e[8] = 0x80u;
                wr_le_u16((owner) + (2u), 100u);
                wr_le_u16((owner) + (4u), 0x60u);
                wr_le_u16((owner) + (8u), 3u);
                memset(mid, 0, 0x12u);
                wr_le_u16((mid) + (2u), 68u);
                wr_le_u16((mid) + (4u), 0x60u);
                wr_le_u16((mid) + (8u), 3u);
                object_group_dissolve_at(0u);
                _check(rd_le_u16((owner) + (2u)) == 92u &&
                           rd_le_u16((owner) + (4u)) == 0x10u &&
                           rd_le_u16((owner) + (8u)) == 2u,
                       "a 3-stack dissolve stands the first member (D-231)");
                _check(rd_le_u16((top) + (8u)) == 3u &&
                           rd_le_u16((mid) + (8u)) == 3u,
                       "and resets the middle/top members (D-231)");
                memset(mid, 0, 0x12u);
            }
            memset(owner, 0, 0x12u);
            memset(top, 0, 0x12u);
        }

        /* D-156: the 0xB6 record's +0xC link is the *next* live record, in
         * the same `record - objArray + 4` encoding objLinkRecord expects. */
        {
            static uint8_t b6map[640];
            uint8_t *b6 = NULL, *next;

            memset(b6map, 0, sizeof b6map);
            b6map[0] = 0xB6u;
            object_init();
            _check(object_load_terrain_objects(b6map) == 9u,
                   "the 0xB6 tile increments the live count");
            for (uint32_t i = 0; i < 8u; i++)
                if (rd_le_u16((object_slot(i)) + (8u)) == 0x1Du)
                    b6 = object_slot(i);
            _check(b6 != NULL, "the 0xB6 static record exists");
            next = object_link_for_probe(rd_le_u16((b6) + (0xCu)));
            _check(next == object_slot(9u),
                   "the 0xB6 link resolves the next live record (D-156)");
            wr_le_u16((b6) + (0xAu), 0u);
            wr_le_u16((b6) + (4u), 0xFEu);
            b6[6] = 0u;
            next[6] = 0x5Au;
            _check(object_command_run(b6, 0x01u) == OBJ_CMD_TAIL,
                   "the 0xB6 type-29 tail runs");
            _check(rd_le_u16((next) + (4u)) == 0x102u &&
                       rd_le_u16((next) + (0u)) ==
                           (uint16_t)(rd_le_u16((b6) + (0u)) + 0x0Fu) &&
                       rd_le_u16((next) + (2u)) ==
                           (uint16_t)(rd_le_u16((b6) + (2u)) - 0x0Cu) &&
                       next[6] == 0u,
                   "the type-29 tail writes the linked record (D-156)");
        }

        /* D-123: the 0xB8/0xBB spawns store the matched map cell itself. */
        {
            static uint8_t m[640];
            uint8_t *b8 = NULL, *bb = NULL;

            memset(m, 0, sizeof m);
            m[2u * 32u + 3u] = 0xB8u;
            m[3u * 32u + 4u] = 0xBBu;
            object_init();
            (void)object_load_terrain_objects(m);
            for (uint32_t i = 0; i <= object_live_count(); i++) {
                uint8_t *r = object_slot(i);
                if (rd_le_u16((r) + (4u)) == 0x00AFu) b8 = r;
                if (rd_le_u16((r) + (4u)) == 0x0026u) bb = r;
            }
            _check(b8 != NULL && rd_le_u16((b8) + (0xEu)) == 2u * 32u + 3u,
                   "the 0xB8 record stores its own map cell (D-123)");
            _check(bb != NULL && rd_le_u16((bb) + (8u)) == 3u * 32u + 4u,
                   "the 0xBB record stores its own map cell (D-123)");
        }

        /* D-152/D-155/D-118: the span flag, the height==distance hold and the
         * matched-entry free. */
        {
            uint8_t *a = object_slot(0);
            uint8_t *s0 = object_span_at_for_probe(0);
            uint8_t *s1 = object_span_at_for_probe(1);

            _check(object_span_flag_for_probe(5, 5u) == 0xFFFFu &&
                       object_span_flag_for_probe(6, 5u) == 1u &&
                       object_span_flag_for_probe(4, 5u) == 0u,
                   "the span flag is 0xFFFF/1/0 (D-152)");
            object_init();
            object_set_action_entity(0u);
            a = object_slot(0);
            s0 = object_span_at_for_probe(0);
            s1 = object_span_at_for_probe(1);
            a[8] = 9u; /* type 9 -> the 0xDB48 tail */
            wr_le_u16((a) + (4u), 0x60u);
            /* D-118: only the second entry is owned; it must be the freed one */
            memset(s0, 0, 0x1Cu);
            memset(s1, 0, 0x1Cu);
            wr_le_u16((s0) + (0u), 0x1234u);
            wr_le_u16((s1) + (0u), 0u); /* actor index 0 */
            wr_le_u16((s1) + (0x12u), 0xFFFFu);
            wr_le_u16((s1) + (8u), 0u);
            (void)object_command_run(a, 0x01u);
            _check(rd_le_u16((s1) + (0u)) == 0xFFFFu &&
                       rd_le_u16((s0) + (0u)) == 0x1234u &&
                       rd_le_u16((a) + (4u)) == 0u &&
                       rd_le_u16((a) + (8u)) == 0u,
                   "the owner reset frees the matched span entry (D-118)");
            /* D-155: a matched but gated entry with height == distance holds */
            object_init();
            a = object_slot(0);
            s0 = object_span_at_for_probe(0);
            object_set_action_entity(0u);
            a[8] = 9u;
            wr_le_u16((a) + (4u), 0x60u);
            wr_le_u16((a) + (0xAu), 7u);
            memset(s0, 0, 0x1Cu);
            wr_le_u16((s0) + (0u), 0u);
            wr_le_u16((s0) + (0x12u), 0u); /* gate fails -> DBAE */
            wr_le_u16((s0) + (6u), 5u);
            wr_le_u16((s0) + (8u), 5u);
            (void)object_command_run(a, 0x01u);
            _check(rd_le_u16((a) + (4u)) == 0x60u &&
                       rd_le_u16((a) + (0xAu)) == 7u &&
                       rd_le_u16((s0) + (0u)) == 0u,
                   "the span height == distance holds the frame (D-155)");
            wr_le_u16((s0) + (8u), 3u); /* now the step runs */
            wr_le_u16((a) + (0xAu), 0u);
            (void)object_command_run(a, 0x01u);
            _check(rd_le_u16((a) + (4u)) == 0x5Fu &&
                       rd_le_u16((a) + (0xAu)) == 3u,
                   "a differing span height lets the 3-beat step run (D-155)");
        }
        object_init();

        /* 1000:DD3C, the give exit: when the record in the window is the
         * player's, the actor's class slot is freed and the actor is re-framed
         * at that class's placed record (frame 0x103, type 0x1E, +0xA = 6). */
        {
            uint8_t *node = object_slot(1u);
            uint8_t *target = object_slot(2u);
            uint8_t *slotRec = object_slot(9u);

            wr_le_u16((a) + (8u), 0x0Fu);
            a[7] = 1u; /* class id 1 -> record 8+1 */
            wr_le_u16((a) + (0xEu), 1u * 0x12u + 4u);
            memset(node, 0, 0x12u);
            wr_le_u16((node) + (0u), 100u);
            wr_le_u16((node) + (2u), 100u);
            wr_le_u16((node) + (8u), 1u);
            memset(target, 0, 0x12u);
            wr_le_u16((target) + (0u), 108u);
            wr_le_u16((target) + (2u), 92u);
            wr_le_u16((target) + (8u), 1u);
            memset(slotRec, 0, 0x12u);
            wr_le_u16((slotRec) + (0u), 200u);
            wr_le_u16((slotRec) + (2u), 200u);
            object_set_player(2u);
            (void)object_command_run(a, 0x01u);
            _check(rd_le_u16((a) + (0u)) == 200u &&
                       rd_le_u16((a) + (2u)) == 192u,
                   "the give exit takes the class record's position, y - 8");
            _check(rd_le_u16((a) + (4u)) == 0x103u &&
                       rd_le_u16((a) + (8u)) == 0x1Eu,
                   "the give exit sets frame 0x103 / type 0x1E");
            _check(a[7] == 0u && rd_le_u16((slotRec) + (0xEu)) == 0xFFFFu,
                   "the class slot is freed and its record marked removed");
            object_set_player(-1);
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

            object_set_action_latched(0);
            wr_le_u16((a) + (8u), 0x17u);
            wr_le_u16((a) + (4u), 0x20u);
            wr_le_u16((a) + (0u), 106u);
            wr_le_u16((a) + (2u), 100u);
            wr_le_u16((a) + (0xEu), 0u);
            _check(object_command_run(a, 0x81u) != OBJ_CMD_UNPORTED &&
                       rd_le_u16((a) + (0xEu)) == 4u,
                   "type 0x17 half-1 command 0 arms the turn (+0xE = 4)");
            if (m)
                m[198] = 0xF0u; /* 2*(rowY-0x10) + (cx>>4), cx=108, rowY=112 */
            _check(object_command_run(a, 0x01u) != OBJ_CMD_UNPORTED,
                   "the latched idle command runs the leave sequence");
            /* frame 0x20 != 0xBB, so the leave nudges LEFT (x -= 4) and then
             * xoff = -2: AX = 100 (1000:F736).  The splash x is
             * ((AX + 8) & ~0xF) - 4 + 8 = 100 (1000:F772) - reading the probe
             * CX instead would give 116.  y = rowY - 0x1C = 84. */
            _check(
                rd_le_u16((a) + (0u)) == 100u && rd_le_u16((a) + (2u)) == 84u,
                "the platform branch snaps to AX, not CX (1000:F772): x 100, y 84");
            _check(rd_le_u16((a) + (4u)) == 0xCDu &&
                       rd_le_u16((a) + (8u)) == 0x1Cu,
                   "the platform branch sets frame 0xCD / type 0x1C");
            _check(rd_le_u16((a) + (0xAu)) == 9u && object_last_sfx() == 6u,
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

            wr_le_u16((a) + (8u), 0u);
            wr_le_u16((a) + (4u), 0x10u);
            wr_le_u16((a) + (0u), 0x40u);
            wr_le_u16((a) + (2u), 0x40u);
            object_set_slot_class(1u, 4u); /* live record 9's class */
            wr_le_u16((nb) + (0u), 0x46u); /* the window of the probe */
            wr_le_u16((nb) + (2u), 0x44u);
            wr_le_u16((nb) + (8u), 0x20u); /* |type| > 0xA: skipped */
            wr_le_u16((nb) + (6u), 0u);
            _check(object_command_run(a, 0x88u) != OBJ_CMD_UNPORTED &&
                       rd_le_u16((nb) + (8u)) == 0x20u,
                   "a neighbour with |type| > 0xA is skipped");
            wr_le_u16((nb) + (8u), 0x8u);
            wr_le_u16((a) + (0u), 0x40u);
            (void)object_command_run(a, 0x88u);
            _check(rd_le_u16((nb) + (8u)) == 0x8u ||
                       rd_le_u16((nb) + (8u)) == 0x12u,
                   "a neighbour under the gate takes +0xA or keeps its type");
            object_set_slot_class(1u, 5u); /* restore the spear's class */
        }
    }

    /* 8. the camera follower (1000:5CB6): target = record - (0x88, 0x44),
     * clamped to the pan range (0xC0, 0xA8), 4 px per axis per frame; the
     * record's offset pair ([0x2D90] -> 0x2D92) is {0,0} at rest */
    {
        uint8_t *rec = object_slot(0u);

        object_set_action_entity(0u);
        level_set_view(0u, 0u);
        /* 1000:5D37's walk is idle while [0x2D8E]'s bit 15 is set; the fall
         * machine may have cleared it in an earlier test, so start from rest. */
        level_set_offset_word(0xFFFFu);
        level_reset_offset_walk();
        rec[0] = (uint8_t)(0x88u + 0x20u);
        rec[1] = 0x00u; /* (0x20,0x10) */
        rec[2] = (uint8_t)(0x44u + 0x10u);
        rec[3] = 0x00u;
        level_follow_view();
        _check(level_view_x() == 4u && level_view_y() == 4u,
               "the follower moves 4 px an axis (0x5D09/0x5D21)");
        for (uint32_t i = 0; i < 16u; i++) level_follow_view();
        _check(level_view_x() == 0x20u && level_view_y() == 0x10u,
               "the follower settles on record - (0x88, 0x44)");

        rec[0] = 0x68u;
        rec[1] = 0x01u; /* 0x0168: past 0xC0+0x88 */
        rec[2] = 0x04u;
        rec[3] = 0x01u; /* 0x0104: past 0xA8+0x44 */
        for (uint32_t i = 0; i < 64u; i++) level_follow_view();
        _check(level_view_x() == 0xC0u && level_view_y() == 0xA8u,
               "the pan clamps to 512-320 px x 320-152 rows");

        rec[0] = 0x10u;
        rec[1] = 0x00u; /* left of the anchor */
        rec[2] = 0x10u;
        rec[3] = 0x00u;
        for (uint32_t i = 0; i < 64u; i++) level_follow_view();
        _check(level_view_x() == 0u && level_view_y() == 0u,
               "a record left of the anchor pins the pan at 0");
        object_set_action_entity(0u);
        level_set_view(0u, 0u);
    }

    /* 8b. 1000:5D37, the follower's offset walk.  The pair at [0x2D90] is added
     * to the record's position before the clamp, and the pointer advances one
     * pair per call while [0x2D8E]'s bit 15 is clear; on the last pair (0x2DA2)
     * the original writes 0x2D92 into the *gate* and decrements it, leaving the
     * pointer there - so the camera keeps the (1, 1) offset for good.  That is
     * the write objFallMachine makes at 0xF8CE (`[0x2D8E] = vx >> 3`). */
    {
        uint8_t *rec = object_slot(0u);

        object_set_action_entity(0u);
        wr_le_u16((rec) + (0u), 0x88u + 0x20u);
        wr_le_u16((rec) + (2u), 0x44u + 0x10u);
        level_set_view(0u, 0u);
        level_set_offset_word(0xFFFFu); /* bit 15: the walk idles */
        level_reset_offset_walk();
        for (uint32_t i = 0; i < 16u; i++) level_follow_view();
        _check(level_view_x() == 0x20u && level_view_y() == 0x10u,
               "an idle gate leaves the follower's target alone");
        /* 0xF8CE's write: a small vertical speed clears bit 15 */
        level_set_offset_word(0x0000u);
        for (uint32_t i = 0; i < 16u; i++) level_follow_view();
        _check(
            level_view_x() == 0x21u && level_view_y() == 0x11u,
            "the cleared gate walks the pairs and shifts the camera to (1, 1)");
        _check(level_offset_word() == 0x2D91u,
               "the last pair writes 0x2D91 into the gate (1000:5D47/5D4D)");
    }

    /* 10. 1000:5C2F, the two TER.DAT slots.  object_array_init (0x4CAB) hides
     * them with 0xFFFF in their +0x10 flag words - the live game reads exactly
     * that (00 x16, ff ff, 00 00 in both slots) - so nothing is drawn until a
     * slot is enabled by hand. */
    {
        static uint8_t page[VIDEO_PAGE_BYTES];
        uint8_t *s0 = object_ter_slot_at(0u);

        _check(s0 && (rd_le_u16(s0 + 0x10)) == 0xFFFFu,
               "1000:4CAB disables both TER slots (0x64AA/0x64BE = 0xFFFF)");
        memset(page, 0, sizeof page);
        object_render_ter(page, 0, 0);
        {
            uint32_t nz = 0;

            for (uint32_t i = 0; i < sizeof page; i++)
                if (page[i]) nz++;
            _check(nz == 0u, "a disabled slot draws nothing (1000:587F)");
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
        object_render_ter(page, 0, 0);
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
            _check(inside != 0u && outside == 0u,
                   "an enabled slot draws the 32x24 cell at (x-0xC, y+yoff)");
        }
        s0[0x10] = 0xFFu;
        s0[0x11] = 0xFFu; /* back off */
    }

    /* 10b. D-129 (closed 2026-11-05): 1000:5310/0x532F fill the 0x649A TER
     * slot from an 0xAF+0xB0 tile pair, and update_world's second call
     * (1000:80C0 -> 1000:C7A0/0xC7A9) patrols it between the two cells.
     * Level 4 has 0xAF at (14,12) = (224,192) and 0xB0 at (23,13) =
     * (368,208). */
    if (level_load(4) == 0) {
        uint8_t *s0 = object_ter_slot_at(0u);
        uint8_t *s1 = object_ter_slot_at(1u);

        _check(s0 && rd_le_u16(s0 + 0x10u) == 0u,
               "D-129: the 0xAF tile enables TER slot 0 (clears +0x10)");
        _check(s0 && rd_le_u16(s0 + 0x0u) == 224u &&
                   rd_le_u16(s0 + 0x2u) == 192u &&
                   rd_le_u16(s0 + 0x4u) == 224u && rd_le_u16(s0 + 0x6u) == 192u,
               "D-129: the slot starts at the 0xAF cell");
        _check(s0 && rd_le_u16(s0 + 0x8u) == 368u &&
                   rd_le_u16(s0 + 0xAu) == 208u,
               "D-129: the 0xB0 cell is the secondary target (+8/+0xA)");
        _check(s1 && rd_le_u16(s1 + 0x10u) == 0xFFFFu,
               "D-129: a level with no 0xB1 leaves slot 1 disabled");
        /* the wiring: the world frame runs it (1000:80C0), so driving the
         * start chain moves the slot without calling the tick by hand */
        {
            uint32_t moved = 0;

            for (uint32_t i = 0; i < 400u && !moved; i++) {
                level_begin();
                level_tick();
                palette_tick();
                if (level_loading_screen()) level_loading_tick();
                if (s0 && rd_le_u16(s0 + 0x0u) != 224u) moved = 1;
            }
            _check(moved != 0u,
                   "D-129: the world frame advances the slot (1000:80C0)");
        }
        /* the interpolation itself, on a fresh slot */
        if (level_load(4) == 0 && s0) {
            object_camera_slots_tick();
            _check(rd_le_u16(s0 + 0x0u) == 225u && rd_le_u16(s0 + 0x2u) == 193u,
                   "D-129: one tick steps one pixel on both axes");
            _check(rd_le_u16(s0 + 0xCu) == 6u,
                   "D-129: the first +0xE wrap bumps the +0xC sprite index");
            for (uint32_t i = 0; i < 143u; i++) object_camera_slots_tick();
            _check(rd_le_u16(s0 + 0x0u) == 368u && rd_le_u16(s0 + 0x2u) == 208u,
                   "D-129: 144 ticks reach the 0xB0 cell");
            object_camera_slots_tick(); /* arrives: the phase toggles */
            _check(rd_le_u16(s0 + 0x10u) == 0x00FFu,
                   "D-129: arrival toggles +0x10's low byte");
            object_camera_slots_tick();
            _check(rd_le_u16(s0 + 0x0u) == 367u && rd_le_u16(s0 + 0x2u) == 207u,
                   "D-129: the next frame patrols back toward 0xAF");
        }
        /* D-226 (closed 2026-11-06): the stack masks ARE the TER slots' +0x12
         * high bytes (0x64AD/0x64C1), and 1000:C8A4 carries every record whose
         * bit is set by the slot's per-frame delta. */
        if (level_load(4) == 0 && s0) {
            uint8_t *base = object_slot(0u);
            uint8_t *top = object_slot(1u);
            uint16_t x0, y0, dx, dy;

            /* base: a registered 0x6F22 group at the 0xAF cell */
            _tf_wr16(base, 0u, 224u);
            _tf_wr16(base, 2u, 192u);
            _tf_wr16(base, 4u, 0x0010u);
            _tf_wr16(base, 6u, 0u);
            _tf_wr16(base, 8u, 2u);
            object_set_action_entity(0u);
            object_action_set_program(1u);
            object_action_set_cursor(2u);
            object_action_run(); /* op 1 registers the group */
            /* seed the base's bit as the live original had it; the mask byte
             * is the TER slot's +0x12 high byte (0x64AD for slot 0) */
            _check(s0[0x13] == 0u, "D-226: the slot's carry byte starts clear");
            s0[0x13] = 0x80u; /* record 0 (the base) */
            /* a human two 16-px rows above the base is pushed onto it */
            _tf_wr16(top, 0u, 224u);
            _tf_wr16(top, 2u, 164u);
            _tf_wr16(top, 4u, 0x0010u);
            _tf_wr16(top, 6u, 0u);
            _tf_wr16(top, 8u, 2u);
            object_set_action_entity(1u);
            _check(object_stack_push(top) == 1,
                   "D-226: the push takes the human above the base");
            _check(s0[0x13] == 0xC0u && s0[0x12] == 0u,
                   "D-226: the push marks the member in the TER carry byte");

            /* the patrol now carries both marked records */
            _tf_wr16(base, 0u, 100u);
            _tf_wr16(base, 2u, 100u);
            _tf_wr16(top, 0u, 120u);
            _tf_wr16(top, 2u, 120u);
            x0 = rd_le_u16(s0 + 0u);
            y0 = rd_le_u16(s0 + 2u);
            object_camera_slots_tick();
            dx = (uint16_t)(rd_le_u16(s0 + 0u) - x0);
            dy = (uint16_t)(rd_le_u16(s0 + 2u) - y0);
            _check(dx != 0u || dy != 0u, "D-226: the slot patrols");
            _check(rd_le_u16(base + 0u) == (uint16_t)(100u + dx) &&
                       rd_le_u16(base + 2u) == (uint16_t)(100u + dy),
                   "D-226: the base record rides the slot delta");
            _check(rd_le_u16(top + 0u) == (uint16_t)(120u + dx) &&
                       rd_le_u16(top + 2u) == (uint16_t)(120u + dy),
                   "D-226: the pushed member rides the slot delta");
        }
        /* the reported level: 27 (NICENEASY) has 0xAF at (9,18) and 0xB0 at
         * (11,11), so the slot starts at (144,288) and patrols up.  The live
         * rider was the acting record 2 at (168,233) with slot 0 at (176,241):
         * the gate's box is (slot.x - 8, slot.y - 8). */
        if (level_load(26) == 0) {
            uint8_t *s = object_ter_slot_at(0u);
            uint8_t *r = object_slot(2u);
            uint16_t sx = rd_le_u16(s + 0u), sy = rd_le_u16(s + 2u);

            _check(s && rd_le_u16(s + 0x10u) == 0u && sx == 144u && sy == 288u,
                   "D-226: level 27 enables TER slot 0 at the 0xAF cell");
            memset(r, 0, 0x12u);
            _tf_wr16(r, 0u, (uint16_t)(sx - 8u)); /* the live offset */
            _tf_wr16(r, 2u, (uint16_t)(sy - 8u));
            _tf_wr16(r, 4u, 0x0010u);
            _tf_wr16(r, 8u, 2u); /* type 2 -> bit 0x20 */
            _check(object_settle_gate_for_probe(r) == 0 && s[0x13] == 0x20u,
                   "D-226: level 27's TER box marks record 2 (0x20)");
            object_camera_slots_tick();
            _check(rd_le_u16(r + 0u) == (uint16_t)((uint16_t)(sx - 8u) +
                                                   (rd_le_u16(s + 0u) - sx)) &&
                       rd_le_u16(r + 2u) ==
                           (uint16_t)((uint16_t)(sy - 8u) +
                                      (rd_le_u16(s + 2u) - sy)),
                   "D-226: record 2 rides level 27's pterodactyl slot");
            /* and it stays on for a full ride: the gate re-marks the box every
             * frame, the camera tick carries the same delta. */
            {
                uint32_t stay = 0;

                for (uint32_t k = 0; k < 24u; k++) {
                    uint16_t ox =
                        (uint16_t)(rd_le_u16(s + 0u) - rd_le_u16(r + 0u));
                    uint16_t oy =
                        (uint16_t)(rd_le_u16(s + 2u) - rd_le_u16(r + 2u));

                    if (object_settle_gate_for_probe(r) != 0) break;
                    object_camera_slots_tick();
                    if ((uint16_t)(rd_le_u16(s + 0u) - rd_le_u16(r + 0u)) !=
                            ox ||
                        (uint16_t)(rd_le_u16(s + 2u) - rd_le_u16(r + 2u)) != oy)
                        break;
                    stay++;
                }
                _check(stay == 24u,
                       "D-227: the rider stays on the slot for 24 frames");
            }
        }
    }

    /* 9. menu_loop's MAP screen: the card loads, the high-score insert runs
     * first when the running total qualifies (1000:01CD), then the key exits. */
    hiscore_reset();
    _check(map_begin() == 1, "the MAP screen starts");

    /* D-34 (closed 2026-11-05): the page origin is high.bit's stone slab, not
     * the band sheet.  The old 1000:12D3-at-the-origin fill painted 0x0F
     * where a band bit was set; both corners below are high.bit's 0. */
    {
        char hp[512];
        snprintf(hp, sizeof hp, "%s/HUMANS.000/LBM/HIGH.BIT", fix_dir);
        FILE *hf = fopen(hp, "rb");
        if (hf) {
            static uint8_t hb[320u * 200u];
            size_t n = fread(hb, 1, sizeof hb, hf);
            fclose(hf);
            if (n == sizeof hb) {
                const uint8_t *pg = osys_null_page(0);
                _check(pg[3u * 320u + 315u] == hb[3u * 320u + 315u] &&
                           pg[5u * 320u + 300u] == hb[5u * 320u + 300u],
                       "D-34: the page origin is high.bit, not the band fill");
            }
        }
    }
    /* D-34: the band staging page (1000:12D3) and the four-strip animation
     * (1000:07B2/09DA/0A72/0E77/0EBE/0EEC/0F3E).  The strips sit at
     * x = 96+32*ch, y = 146..185, under the page art. */
    {
        static uint8_t band[24320];
        char bp[512];
        snprintf(bp, sizeof bp, "%s/HUMANS.000/APL/BAND.SPR", fix_dir);
        FILE *bf = fopen(bp, "rb");
        size_t bn = bf ? fread(band, 1, sizeof band, bf) : 0;
        if (bf) fclose(bf);

        _check(map_stage_ready_for_probe() == 1,
               "D-34: the band stage is filled (1000:12D3)");
        if (bn == sizeof band) {
            uint32_t bad = 0;

            for (uint32_t i = 0; i < 6080u; i += 97u)
                for (uint32_t p = 0; p < 4u; p++)
                    if (map_stage_at(i, p) != band[i * 4u + p]) bad++;
            _check(bad == 0u,
                   "D-34: the stage is the sheet's 4-plane interleave");
        }
        _check(map_anim_channel_for_probe(0u) == 1u &&
                   map_anim_channel_for_probe(1u) == 0u &&
                   map_anim_channel_for_probe(2u) == 0x0100u &&
                   map_anim_channel_for_probe(3u) == 0u,
               "D-34: 1000:07B2 seeds [0x90]=1 and [0x94]=0x100");
        {
            const uint8_t *pg = osys_null_page(0);
            uint32_t bad2 = 0, n2 = 0;

            for (uint32_t ch = 0; ch < 4u; ch++) {
                uint32_t v = map_anim_channel_for_probe(ch);
                uint32_t phase = v + (ch == 0u   ? 0u
                                      : ch == 1u ? 3u
                                      : ch == 2u ? 8u
                                                 : 0x0Eu);

                for (uint32_t r = 0; r < 40u; r++)
                    for (uint32_t c = 0; c < 8u; c++) {
                        uint32_t a = (phase & 0xFFu) * 0x140u + r * 8u + c;
                        uint32_t x = 96u + 32u * ch + c * 4u;
                        uint32_t y = 146u + r;

                        for (uint32_t j = 0; j < 4u; j++) {
                            /* the four plane bytes are the four pixels'
                             * full colours (measured live, 2026-11-05) */
                            n2++;
                            if (pg[y * 320u + x + j] != map_stage_at(a, j))
                                bad2++;
                        }
                    }
            }
            _check(n2 != 0u && bad2 == 0u,
                   "D-34: the four strips carry the staged pixels");
        }
        for (uint32_t i = 0; i < 10u; i++) map_anim_tick_for_probe();
        _check(map_anim_channel_for_probe(2u) == 1u,
               "D-34: 1000:0E77 steps [0x94] 0x100 -> 1 after 10 frames");
    }

    {
        InputState k;
        memset(&k, 0, sizeof k);
        k.lastKey = 1;
        _check(map_tick(&k) == 1,
               "the MAP's first tick ignores the cutscene's held key");
        /* the score screen's `prepared` total qualifies, so the name grid is
         * up.  Type two letters, delete one, type again, then Enter commits. */
        {
            uint32_t total =
                ((uint32_t)score_total_hi() << 16) | score_total_lo();
            int32_t found = 0;

            _check(hiscore_qualifies(total),
                   "1000:01CD: the running total qualifies for the table");
            /* 1000:024B: a qualifying total sees the seven-line page first;
             * the key that dismisses it is not typed into the name. */
            {
                static uint8_t page[VIDEO_PAGE_BYTES];
                uint32_t lit = 0;

                memset(page, 0, sizeof page);
                map_draw_congrats(page);
                for (uint32_t i = 0; i < 7u; i++) {
                    uint32_t y = 0x19u + 0x10u * i;
                    uint32_t n = 0;
                    for (uint32_t r = 4u; r < 14u; r++)
                        for (uint32_t x = 0; x < 320u; x++)
                            if (page[(y + r) * 320u + x]) n++;
                    _check(n != 0u,
                           "1000:024B: a congratulation line is drawn");
                }
                for (uint32_t i = 0; i < sizeof page; i++)
                    if (page[i]) lit++;
                _check(lit != 0u, "the congratulation page draws text");
            }
            /* D-206 (closed 2026-11-05): the opening chain 1000:021E..0x0285
             * fades the band's 0x10..0x1F entries in, runs 0x8C animates,
             * fades 0x20..0x3F to black, re-copies high.bit, draws the text
             * twice, then fades 0..0x3F in.  Drive map_tick + palette_tick. */
            {
                uint32_t guard = 0;
                uint32_t step_mid = 0xFFFFFFFFu;
                uint8_t pal0[768];
                const uint8_t *work = palette_working();
                int32_t band_up = 0, table_down = 0;

                memcpy(pal0, osys_null_palette(), sizeof pal0);
                k.lastKey = 0;
                while (map_intro_step_for_probe() < 4u && guard++ < 4000u) {
                    map_tick(&k);
                    palette_tick();
                    if (guard == 0x60u) step_mid = map_intro_step_for_probe();
                    /* D-217: after the 0x0224 fade has had its 0x1E ticks and
                     * while 0x0233 still runs, the strips' 0x10..0x1F half is
                     * at the working palette and the stone table's 0x00..0x0F
                     * is exactly the incoming DAC. */
                    if (guard == 0x30u && map_intro_step_for_probe() == 0u) {
                        const uint8_t *pal = osys_null_palette();
                        uint32_t bad_lo = 0, bad_hi = 0;

                        for (uint32_t i = 0; i < 0x30u; i++)
                            bad_lo += pal[i] != pal0[i];
                        for (uint32_t i = 0; i < 0x30u; i++)
                            bad_hi += pal[0x30u + i] != work[0x30u + i];
                        table_down = (bad_lo == 0u);
                        band_up = (bad_hi == 0u);
                    }
                }
                _check(guard < 4000u && map_intro_step_for_probe() == 4u,
                       "D-206: the opening fade chain reaches the key wait");
                _check(band_up && table_down,
                       "D-217: 1000:0224 fades only the strips' 0x10..0x1F");
                _check(step_mid == 0u,
                       "D-217: 1000:0233 runs 0x8C animate frames over ticks");
                _check(map_congrats_draws() == 2u,
                       "1000:0251: the two congratulation passes are drawn");
                {
                    const uint8_t *pal = osys_null_palette();
                    uint32_t bad_t = 0, bad_f = 0;

                    /* D-217: the 0x0273 0x00..0x3F fade lifts the stone table
                     * (0x00..0x0F) and the congrats ink (0x20..0x2C) together. */
                    for (uint32_t i = 0; i < 0x30u; i++)
                        bad_t += pal[i] != work[i];
                    for (uint32_t i = 0x60u; i < 0x90u; i++)
                        bad_f += pal[i] != work[i];
                    _check(
                        bad_t == 0u && bad_f == 0u,
                        "D-217: 1000:0273 brings table and font up together");
                }
                /* D-211 (closed 2026-11-05): the display strips survive the
                 * 0x024B offscreen copy; the port must redraw all four. */
                {
                    const uint8_t *pg = osys_null_page(0);
                    uint32_t bad = 0, n = 0;

                    for (uint32_t ch = 0; ch < 4u; ch++) {
                        uint32_t phase = map_anim_channel_for_probe(ch) +
                                         (ch == 0u   ? 0u
                                          : ch == 1u ? 3u
                                          : ch == 2u ? 8u
                                                     : 0x0Eu);

                        for (uint32_t r = 0; r < 40u; r++)
                            for (uint32_t c = 0; c < 8u; c++) {
                                uint32_t a =
                                    (phase & 0xFFu) * 0x140u + r * 8u + c;
                                uint32_t x = 96u + 32u * ch + c * 4u;
                                uint32_t y = 146u + r;

                                for (uint32_t j = 0; j < 4u; j++) {
                                    n++;
                                    if (pg[y * 320u + x + j] !=
                                        map_stage_at(a, j))
                                        bad++;
                                }
                            }
                    }
                    _check(
                        n != 0u && bad == 0u,
                        "D-211: the four band strips survive the opening copy");
                }
            }
            {
                uint32_t f = 0, e = 0, d = 0, t = 0;

                _check(map_fade_at(0u, &f, &e, &d, &t) && f == 0x10u &&
                           e == 0x10u && d == 0x1Eu && t == 0x30u,
                       "D-217: 1000:0224 fades 0x10..0x1F (working+0x30) in");
                _check(map_fade_at(1u, &f, &e, &d, &t) && f == 0x20u &&
                           e == 0x20u && d == 0x02u,
                       "D-206: 1000:0239 fades 0x20..0x3F to black");
                _check(map_fade_at(2u, &f, &e, &d, &t) && f == 0x00u &&
                           e == 0x40u && d == 0x28u && t == 0x00u,
                       "D-206: 1000:0273 fades the panel in");
            }

            /* 1000:028C..030A: the key starts five palette fades, the
             * HIGH.BIT re-copy and the two rectangles; the ring comes up
             * after the fifth.  Drive map_tick + palette_tick together. */
            {
                uint32_t f0 = map_fade_starts();
                uint32_t guard = 0;

                k.lastKey = 'Q';
                map_tick(&k); /* dismiss the page */
                k.lastKey = 0;
                while (map_transition_step() < 6u && guard++ < 2000u) {
                    map_tick(&k);
                    palette_tick();
                }
                _check(guard < 2000u && map_transition_step() == 6u,
                       "1000:028C..030A: the name transition completes");
                _check(map_fade_starts() - f0 == 5u,
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

                        _check(
                            map_fade_at(f0 + i, &f, &e, &d, &t) &&
                                f == want[i][0] && e == want[i][1] &&
                                d == want[i][2] && t == want[i][3],
                            "1000:028C..0307: the fade parameters are the listing's");
                    }
                }
            }
            /* D-205 (closed 2026-11-05): 1000:0310..0x0342 spins the ring one
             * full revolution (20 x 0x12 = 0x168) and then holds for the three
             * 0x0330 draws before the key loop. */
            {
                uint32_t a0 = map_name_angle_for_probe();
                uint32_t moved = 0;

                _check(a0 == 0x10Eu,
                       "D-205: the ring starts at the resting angle 0x10E");
                for (uint32_t i = 0; i < 23u; i++) {
                    uint32_t before = map_name_angle_for_probe();

                    map_tick(&k);
                    if (map_name_angle_for_probe() != before) moved++;
                }
                _check(moved == 20u && map_name_angle_for_probe() == 0x10Eu,
                       "D-205: 20 x 0x12 steps return to 0x10E and hold");
                _check(map_name_col_for_probe() == 0u,
                       "D-205: the rested ring selects cell 0");
                /* D-222 (closed 2026-11-05): 1000:0F93's block table
                 * [0xA8..0xAF] is zero in the image and live at the name
                 * screen, so every cell is block 0 and stays upright.  Compare
                 * the right-hand cell (7) with the unrotated sheet. */
                {
                    static uint8_t packed[0x2000], sheet[0x10000];
                    static uint8_t page[320u * 200u];
                    char ap[512];
                    FILE *af;
                    size_t an = 0;
                    uint16_t src, dst;
                    uint32_t want_ink = 0, ok_ink = 0;

                    snprintf(ap, sizeof ap, "%s/HUMANS.000/APL/ALPHA.SPR",
                             fix_dir);
                    af = fopen(ap, "rb");
                    if (af) {
                        an = fread(packed, 1, sizeof packed, af);
                        fclose(af);
                    }
                    if (an != 0u) {
                        memset(sheet, 0, sizeof sheet);
                        alpha_expand(packed, sheet);
                        alpha_cell(7u, (0x10Eu + 7u * 0x0Cu) % 0x168u, &src,
                                   &dst);
                        memset(page, 0, sizeof page);
                        map_name_set_angle_for_probe(0x10Eu);
                        map_draw_name(page);
                        for (uint32_t r = 0; r < 11u; r++)
                            for (uint32_t c = 0; c < 4u; c++) {
                                uint32_t v =
                                    sheet[(src + r * 4u + c) & 0xFFFFu];

                                for (uint32_t b = 0; b < 4u; b++)
                                    if ((v >> b) & 1u) {
                                        uint32_t px = (dst / 80u + r) * 320u +
                                                      (dst % 80u) * 4u +
                                                      c * 4u + b;

                                        want_ink++;
                                        if (page[px] & 0x20u) ok_ink++;
                                    }
                            }
                        _check(want_ink != 0u && ok_ink * 10u >= want_ink * 9u,
                               "D-222: the wheel uses block 0 (upright)");
                    }
                }
            }
            /* D-208 (closed 2026-11-05): 1000:0CBE's two cell lists.  Draw
             * three angles on a zeroed page; the third draw clears the first
             * one's cells (the 0x0BB0 footprint), so an A-only pixel is gone. */
            {
                static uint8_t pa[VIDEO_PAGE_BYTES], pb[VIDEO_PAGE_BYTES],
                    pc[VIDEO_PAGE_BYTES];
                static uint8_t page[VIDEO_PAGE_BYTES];
                uint32_t a = 0x10Eu, b = a + 6u, c = a + 0x0Cu;
                uint32_t idx = 0xFFFFFFFFu;

                memset(pa, 0, sizeof pa);
                alpha_draw_wheel(pa, a);
                memset(pb, 0, sizeof pb);
                alpha_draw_wheel(pb, b);
                memset(pc, 0, sizeof pc);
                alpha_draw_wheel(pc, c);
                for (uint32_t i = 0; i < 320u * 200u; i++)
                    if (pa[i] && !pb[i] && !pc[i]) {
                        idx = i;
                        break;
                    }
                memset(page, 0, sizeof page);
                map_name_set_angle_for_probe(a);
                map_draw_name(page);
                map_name_set_angle_for_probe(b);
                map_draw_name(page);
                map_name_set_angle_for_probe(c);
                map_draw_name(page);
                _check(idx != 0xFFFFFFFFu && page[idx] == 0u,
                       "D-208: the previous ring cells are erased");
                map_name_set_angle_for_probe(0x10Eu);
            }
            /* D-202 (closed 2026-11-05): left/right move the ring cell and
             * redraw the page (1000:03B5..0x03F8 call 0x0CBE).  The original
             * maps left (0x04) to [0x48] -= 3 x4 -> the next cell, right
             * (0x08) to [0x48] += 3 x4 -> the previous one. */
            {
                static uint8_t before[320u * 200u];
                uint32_t c0 = map_name_col_for_probe();
                uint32_t a0 = map_name_angle_for_probe();

                memcpy(before, osys_null_page(0), sizeof before);
                k.lastKey = 0;
                k.joyBits = 0x08u; /* right */
                map_tick(&k);
                _check(map_name_angle_for_probe() == (a0 + 3u) % 0x168u,
                       "D-213: right starts with one 3-degree draw");
                for (uint32_t i = 0; i < 3u; i++) map_tick(&k);
                _check(map_name_col_for_probe() == (c0 + 29u) % 30u,
                       "D-202: right spins the ring to the previous cell");
                _check(memcmp(before, osys_null_page(0), sizeof before) != 0u,
                       "D-202: the ring is redrawn after the move");
                a0 = map_name_angle_for_probe();
                k.joyBits = 0;
                for (uint32_t i = 0; i < 3u; i++) map_tick(&k); /* 0x0330 */
                _check(map_name_angle_for_probe() == a0,
                       "D-213: the three hold draws do not move the ring");
                k.joyBits = 0x04u; /* left */
                map_tick(&k);
                for (uint32_t i = 0; i < 3u; i++) map_tick(&k);
                _check(map_name_col_for_probe() == c0,
                       "D-202: left moves it back");
                k.joyBits = 0;
                for (uint32_t i = 0; i < 3u; i++) map_tick(&k); /* holds */
            }
            /* D-203 (closed 2026-11-05): the fire append waits for
             * [0x5AC8] >= 0x23 and resets it (1000:03FB/0x0405); the name
             * screen arms it at entry, so the first fire appends and the
             * next immediate one does not. */
            _check(map_name_fire_wait_for_probe() >= 0x23u,
                   "D-203: the name screen arms the fire counter");
            k.joyBits = 0x80u;
            map_tick(&k);
            _check(map_name_for_probe()[0] == 'A' &&
                       map_name_for_probe()[1] == 0,
                   "D-203: the first fire appends the ring cell");
            _check(map_name_fire_wait_for_probe() < 0x23u,
                   "D-203: the fire resets the counter");
            map_tick(&k); /* a second held frame */
            _check(map_name_for_probe()[1] == 0,
                   "D-203: a held fire does not append twice");
            k.joyBits = 0;
            k.lastKey = 8u; /* backspace: delete the 'A' */
            map_tick(&k);
            _check(map_name_for_probe()[0] == 0,
                   "D-203: the typed backspace clears it");
            /* D-214 (closed 2026-11-05): Z is the shipped action key
             * ([0x5B05] = 0x2C).  One typed Z appends once; the still-held
             * fire bit is swallowed until release (1000:0363/0x0368). */
            {
                k.lastKey = 0;
                k.joyBits = 0;
                for (uint32_t i = 0; i < 0x23u; i++)
                    map_tick(&k); /* re-arm [0x5AC8] after D-203's fire */
                k.lastKey = 'Z';
                k.joyBits = 0x80u;
                map_tick(&k);
                _check(map_name_for_probe()[0] == 'Z' &&
                           map_name_for_probe()[1] == 0,
                       "D-214: a typed Z appends once");
                k.lastKey = 0; /* the event layer delivers the make once */
                for (uint32_t i = 0; i < 5u; i++) map_tick(&k);
                _check(map_name_for_probe()[1] == 0,
                       "D-214: the held action bit does not append again");
                k.joyBits = 0;
                map_tick(&k); /* release clears the lock */
                _check(map_name_for_probe()[1] == 0,
                       "D-214: release keeps the name at one Z");
                k.lastKey = 8u; /* clear the Z before D-201 */
                map_tick(&k);
                k.lastKey = 0;
                k.joyBits = 0;
            }
            /* D-201 (closed 2026-11-05): a typed key turns the ring to its
             * cell and then appends (1000:039C..0x03B3). */
            k.lastKey = 'Q';
            map_tick(&k);
            _check(map_name_col_for_probe() == 16u &&
                       map_name_for_probe()[0] == 'Q',
                   "D-201: the typed letter moves the ring and appends");
            /* D-209 (closed 2026-11-05): the appended glyph starts at
             * (0x95,0x16) and slides toward row 0x45 by ≤4 px per draw. */
            {
                int32_t tx = 0, ty = 0, cx = 0, cy = 0;

                _check(map_glyph_for_probe(0u, &tx, &ty, &cx, &cy) &&
                           ty == 0x45 && cy > 0x16 && cy < ty && cx < 0x95,
                       "D-209: the new glyph slides in from (0x95,0x16)");
            }
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
            _check(found, "the typed name and the total reach the table");

            /* D-218 (closed 2026-11-05): 1000:049A..0x0516.  Enter first
             * fades palette 0x20..0x5F to white (div 0x64) while the wheel
             * spins, then back to working[0x60..0x7F] (div 0x3C); only then
             * 0x0518's header is drawn. */
            {
                uint32_t f = 0, e = 0, d = 0, t = 0;
                uint32_t guard = 0, a0 = map_name_angle_for_probe();
                uint32_t white_ok = 0, back_ok = 0;

                _check(
                    map_fade_starts() != 0u &&
                        map_fade_at(map_fade_starts() - 1u, &f, &e, &d, &t) &&
                        f == 0x20u && e == 0x40u && d == 0x64u,
                    "D-218: 1000:04B4 fades 0x20..0x5F to white (0x64)");
                /* BX starts at 1: the first frames step 0, then 1..0x10 */
                for (uint32_t i = 0; i < 12u; i++) {
                    k.lastKey = 0;
                    map_tick(&k);
                    palette_tick();
                }
                _check(map_name_angle_for_probe() != a0,
                       "D-218: the wheel spins during the white fade");
                /* Capture the DAC on the tick the fade went inactive, before
                 * the following map_tick starts the next phase. */
                while (map_enter_step_for_probe() == 0u && guard++ < 600u) {
                    if (!palette_fade_active()) {
                        const uint8_t *pal = osys_null_palette();
                        uint32_t bad = 0;

                        for (uint32_t i = 0x20u; i < 0x60u; i++)
                            if (pal[i * 3] != 0x3Fu ||
                                pal[i * 3 + 1] != 0x3Fu ||
                                pal[i * 3 + 2] != 0x3Fu)
                                bad++;
                        white_ok = (bad == 0u);
                        map_tick(&k); /* the phase-1 transition */
                        break;
                    }
                    map_tick(&k);
                    palette_tick();
                }
                _check(white_ok, "D-218: 1000:04B4 reaches white");
                _check(map_fade_at(map_fade_starts() - 1u, &f, &e, &d, &t) &&
                           f == 0x20u && e == 0x40u && d == 0x3Cu,
                       "D-218: 1000:050E fades 0x20..0x5F back (0x3C)");
                guard = 0;
                while (map_enter_step_for_probe() == 1u && guard++ < 600u) {
                    if (!palette_fade_active()) {
                        const uint8_t *pal = osys_null_palette();
                        const uint8_t *work = palette_working();
                        uint32_t bad = 0;

                        for (uint32_t i = 0; i < 0x40u; i++) {
                            uint32_t w = 0x120u + (i % 0x20u) * 3u;

                            if (pal[0x20u * 3u + i * 3u] != work[w] ||
                                pal[0x20u * 3u + i * 3u + 1u] != work[w + 1u] ||
                                pal[0x20u * 3u + i * 3u + 2u] != work[w + 2u])
                                bad++;
                        }
                        back_ok = (bad == 0u);
                        map_tick(&k); /* the header transition */
                        break;
                    }
                    map_tick(&k);
                    palette_tick();
                }
                _check(back_ok && map_enter_step_for_probe() == 2u,
                       "D-218: 1000:050E returns to working 0x60..0x7F");
            }

            /* draw_score_block (1000:0651) writes the rows into the
             * **offscreen** page (0x0577 clears it first); the display keeps
             * high.bit + the header (D-219). */
            {
                static uint8_t page[VIDEO_PAGE_BYTES];
                static uint8_t high[VIDEO_PAGE_BYTES];
                static const uint32_t cx[3] = { 0x20u, 0x82u, 0x102u };
                char hp[512];
                size_t hn = 0;
                FILE *hf;

                snprintf(hp, sizeof hp, "%s/HUMANS.000/LBM/HIGH.BIT", fix_dir);
                hf = fopen(hp, "rb");
                if (hf) {
                    hn = fread(high, 1, sizeof high, hf);
                    fclose(hf);
                }
                memset(page, 0xFF, sizeof page);
                map_draw_list(page); /* the offscreen page */
                _check(page[150u * 320u + 300u] == 0u,
                       "D-219: 1000:0577 clears the offscreen list page");
                for (uint32_t k = 0; k < 10u; k++) {
                    uint32_t y = k * 0x14u;
                    for (uint32_t c = 0; c < 3u; c++) {
                        uint32_t n = 0;

                        for (uint32_t r = 4u; r < 14u; r++)
                            for (uint32_t x = cx[c] - 16u; x <= cx[c] + 16u;
                                 x++)
                                if (page[(y + r) * 320u + x]) n++;
                        _check(n != 0u,
                               "1000:0651: rank/name/score ink at the column");
                    }
                }
                {
                    uint32_t n = 0;

                    for (uint32_t r = 4u; r < 14u; r++)
                        for (uint32_t x = 0xC0u; x < 0xF0u; x++)
                            if (page[(0x14u + r) * 320u + x]) n++;
                    _check(n == 0u, "D-219: the offscreen page has no header");
                }
                /* D-218: the display page is still high.bit + the title */
                {
                    const uint8_t *disp = osys_null_page(0);
                    uint32_t ink = 0;

                    for (uint32_t r = 4u; r < 14u; r++)
                        for (uint32_t x = 0x80u; x <= 0xC0u; x++)
                            if (disp[(0x14u + r) * 320u + x] & 0x20u) ink++;
                    _check(hn == sizeof high && ink != 0u &&
                               disp[150u * 320u + 300u] ==
                                   high[150u * 320u + 300u],
                           "D-218: the header keeps high.bit and the title");
                }
                /* D-98 (closed 2026-11-05): the 15 outlines live only on the
                 * 0x0518 header frames, before 0x0577 clears the page. */
                memset(page, 0, sizeof page);
                map_draw_list_header(page);
                {
                    uint32_t n = 0;

                    for (uint32_t r = 4u; r < 14u; r++)
                        for (uint32_t x = 0x80u; x <= 0xC0u; x++)
                            if (page[(0x14u + r) * 320u + x]) n++;
                    _check(n != 0u,
                           "1000:054E: the list header is at (0xA0,0x14)");
                    _check((page[0x2Du * 320u + 0x12u] & 0x40u) != 0u &&
                               (page[0x2Du * 320u + 0x32u] & 0x40u) != 0u &&
                               (page[0x2Du * 320u + 0xD6u] & 0x40u) != 0u &&
                               (page[0x7Du * 320u + 0x12u] & 0x40u) != 0u,
                           "D-98: 1000:0B5F draws the 5 x 3 outlines");
                }
                /* 1000:0CBE: the name screen has the wheel and the typed
                 * name only - no list columns, no port-only NAME: line. */
                memset(page, 0, sizeof page);
                map_draw_name(page);
                {
                    uint32_t low = 0, wide = 0, ink = 0;

                    for (uint32_t y = 160u; y < 200u; y++)
                        for (uint32_t x = 0; x < 320u; x++)
                            if (page[y * 320u + x]) low++;
                    for (uint32_t y = 0; y < 160u; y++)
                        for (uint32_t x = 240u; x < 320u; x++)
                            if (page[y * 320u + x]) wide++;
                    for (uint32_t y = 0; y < 160u; y++)
                        for (uint32_t x = 0; x < 240u; x++)
                            if (page[y * 320u + x]) ink++;
                    _check(
                        low == 0u && wide == 0u && ink != 0u,
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
            _check(out[32] == 0u && out[33] == 14u && out[34] == 7u &&
                       out[35] == 0u,
                   "1000:0705: plane 0 of 0xE007 is 0,14,7,0");
            _check(out[0x4000 + 32] == 0u && out[0x4000 + 33] == 12u &&
                       out[0x4000 + 34] == 15u && out[0x4000 + 35] == 0u,
                   "1000:0705: plane 1 is 0,12,15,0");
        }

        /* the ring: rotate_spinner_cell (1000:0F93) with the image's own
         * parameters - sin table at DS:0xB8, [0xB2]/[0xB0] = 157/66 (the x),
         * [0xB6]/[0xB4] = 79/62 (the y), page stride 80. */
        {
            uint16_t src, dst;

            alpha_cell(0u, 0x10Eu, &src, &dst);
            _check(dst == 997u && src == 16396u,
                   "1000:0F93: cell 0 lands at row 12, px 148, plane 1");
            alpha_cell(5u, 0x10Eu + 5u * 0x0Cu, &src, &dst);
            _check(dst == 3491u && src == 33100u,
                   "cell 5 lands at row 43, px 204, plane 3");
            alpha_cell(15u, 0x10Eu + 15u * 0x0Cu, &src, &dst);
            _check(dst == 10917u && src == 17356u,
                   "cell 15 (the far side of the ring) lands at row 136");
        }
        {
            static uint8_t page[VIDEO_PAGE_BYTES];
            uint32_t in = 0, out = 0;

            memset(page, 0, sizeof page);
            alpha_draw_wheel(page, 0x10Eu);
            for (uint32_t i = 0; i < sizeof page; i++)
                if (page[i]) in++;
            _check(in != 0u, "the 30-cell letter wheel draws into the page");
            _flow_dump_ppm("map_ring.ppm", page);
            memset(page, 0, sizeof page);
            alpha_draw_name(page, "AB");
            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x++)
                    if (page[y * 320u + x]) {
                        if (y < 70u || y >= 90u) out++;
                    }
            _check(out == 0u, "the typed name is drawn at row 0x45 (72)");
            _flow_dump_ppm("map_wheel.ppm", page);
            /* the glyph's bits are its pixels: a one-character name draws the
             * 'A' cell (plane 0, x & 3 == 0) - 46 set bits in the shipped
             * sheet's 16x4-byte cell - and nothing else */
            {
                uint32_t lit = 0;

                memset(page, 0, sizeof page);
                alpha_draw_name(page, "A");
                for (uint32_t i = 0; i < sizeof page; i++)
                    if (page[i]) lit++;
                _check(lit == 46u,
                       "the 'A' glyph is 46 pixels of the shipped alpha cell");
            }
            /* D-207 (closed 2026-11-05): the cell writes bit 5 only (write
             * mode 2 + Data Rotate OR + Bit Mask 0x20), so the background's
             * other bits survive and a clear glyph bit leaves the pixel. */
            {
                uint32_t lit = 0, bad = 0;

                memset(page, 0x11, sizeof page);
                alpha_draw_name(page, "A");
                for (uint32_t i = 0; i < sizeof page; i++) {
                    if (page[i] == 0x31u) lit++;
                    else if (page[i] != 0x11u) bad++;
                }
                _check(
                    lit == 46u && bad == 0u,
                    "D-207: the glyph ORs 0x20 and preserves the background");
            }

            /* the cursor spins the ring: the selected column sits at the top */
            {
                uint16_t src, dst, top = 0xFFFFu;

                for (uint32_t i = 0; i < 30u; i++) {
                    uint32_t ang = (0x10Eu - 7u * 0x0Cu + i * 0x0Cu) % 0x168u;

                    alpha_cell(i, ang, &src, &dst);
                    if (dst / 80u < top) top = dst / 80u;
                }
                _check(top <= 20u,
                       "the selected cell sits at the top of the ring");
            }
        }

        /* 1000:1251: the two rectangle outlines (top 0x40, left/bottom 0x50,
         * right 0x40) land at the listing's coordinates. */
        {
            static uint8_t page[VIDEO_PAGE_BYTES];

            memset(page, 0, sizeof page);
            map_draw_rect(page, 0x95u, 9u, 0xFu, 0xFu);
            map_draw_rect(page, 0x60u, 0x45u, 0x77u, 0xFu);
            _check((page[9u * 320u + 0x95u] & 0x40u) != 0u &&
                       (page[(9u + 0xFu) * 320u + 0x95u] & 0x50u) != 0u &&
                       (page[9u * 320u + 0x95u + 0xFu] & 0x40u) != 0u &&
                       (page[20u * 320u + 0x95u] & 0x50u) != 0u,
                   "1000:1251: the first outline is at (0x95,9) 0xF x 0xF");
            _check((page[0x45u * 320u + 0x60u] & 0x40u) != 0u &&
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
            while (g_map_panel == 0u && guard++ < 2000u) {
                map_tick(&k);
                palette_tick();
            }
            _check(guard >= 0x12Eu && g_map_box_bottom == 2u &&
                       g_map_panel == 1u,
                   "1000:0518: the frame unrolls from row 0x130 to row 2");
        }
        /* 1000:05F4/0613: a panel step is 0x14 frames of one [0x8AE] unit
         * each; [0x8A2] changes only after the 0x14th. */
        {
            uint32_t before = g_map_box_bottom;

            k.joyBits = 0x01u;
            map_tick(&k);
            _check(g_map_box_bottom == before && g_map_panel == 1u,
                   "1000:060C: the step tick only arms the 0x14-frame walk");
            k.joyBits = 0;
            for (uint32_t i = 0; i < 0x14u; i++) {
                map_tick(&k);
                if (i + 1u < 0x14u)
                    _check(g_map_box_bottom == before + i + 1u &&
                               g_map_panel == 1u,
                           "1000:0613: the frame moves one unit per tick");
            }
            _check(g_map_box_bottom == before + 0x14u && g_map_panel == 2u,
                   "1000:060C: after 0x14 frames the panel is 2");
            for (uint32_t p = 2u; p < 6u; p++) {
                k.joyBits = 0x01u;
                map_tick(&k);
                k.joyBits = 0;
                for (uint32_t i = 0; i < 0x14u; i++) map_tick(&k);
            }
            _check(g_map_panel == 6u && g_map_box_bottom == 2u + 5u * 0x14u,
                   "the panel stops at 6 with the frame at 2 + 5*0x14");
            /* down reverses the same 0x14-frame walk */
            k.joyBits = 0x02u;
            map_tick(&k);
            k.joyBits = 0;
            for (uint32_t i = 0; i < 0x14u; i++) map_tick(&k);
            _check(
                g_map_panel == 5u && g_map_box_bottom == 2u + 4u * 0x14u,
                "1000:05ED: a down step walks the frame back over 0x14 frames");
        }
        /* 1000:07E6: the five frame rows, at rows 46/66/86/106/126 (the
         * 0xE66/0x14A6/0x1AE6/0x2126/0x2766 offsets on an 80-byte stride),
         * each a 14-row band built from the 4/+7/31/+9/15/+0xE runs.  The
         * source is the offscreen page (1000:0874's DS=[0x8A4]); write mode
         * 2 + the box's Bit Mask sets/clears only the masked colour bits. */
        {
            static uint8_t page[VIDEO_PAGE_BYTES];
            static const uint32_t dstByte[5] = { 0xE66u, 0x14A6u, 0x1AE6u,
                                                 0x2126u, 0x2766u };
            static const uint32_t masks[5] = { 0xA0u, 0xB0u, 0xE0u, 0xF0u,
                                               0x30u };
            static const uint32_t runN[3] = { 4u, 31u, 15u };
            static const uint32_t gapN[3] = { 7u, 9u, 0x0Eu };
            uint8_t *off = map_off_page_for_probe();
            uint32_t checked = 0, bad = 0, stray = 0;

            /* every 4-pixel group is (1,0,1,0), so the packed source byte
             * is 0x05 for every in-range address */
            memset(off, 0, 320u * 200u);
            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x += 4u) {
                    off[y * 320u + x] = 1u;
                    off[y * 320u + x + 2u] = 1u;
                }
            memset(page, 0x08, sizeof page);
            map_draw_box_edges(page, 2u);
            for (uint32_t k = 0; k < 5u; k++) {
                uint32_t d = dstByte[k];
                uint32_t M = masks[k];

                for (uint32_t it = 0; it < 14u; it++)
                    for (uint32_t r = 0; r < 3u; r++) {
                        for (uint32_t n = 0; n < runN[r]; n++) {
                            uint32_t v = 0x05u;
                            uint32_t row = d / 80u, col = d % 80u;

                            for (uint32_t p = 0; p < 4u; p++) {
                                uint8_t *px = &page[row * 320u + col * 4u + p];
                                uint8_t exp =
                                    (uint8_t)((0x08u & (uint8_t)~M) |
                                              (((v >> p) & 1u) ? M : 0u));

                                if (*px != exp) bad++;
                                checked++;
                            }
                            d++;
                        }
                        d += gapN[r];
                    }
            }
            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x++) {
                    int32_t in = 0;

                    for (uint32_t k = 0; k < 5u; k++)
                        if (y >= dstByte[k] / 80u && y < dstByte[k] / 80u + 14u)
                            in = 1;
                    if (!in && page[y * 320u + x] != 0x08u) stray++;
                }
            _check(checked != 0u && bad == 0u,
                   "D-219: the frame blends the offscreen page's bits");
            _check(stray == 0u, "D-210: the frame writes only the five bands");
        }
        _flow_dump_ppm("map_frame.ppm", video_page(0));
        /* D-220 (closed 2026-11-05): the key starts 1000:062B's 0x100-entry
         * fade to black (div 0x23), then the 0x4B-frame hold and the save. */
        {
            uint32_t f = 0, e = 0, d = 0, t = 0, guard = 0;

            k.lastKey = 1;
            _check(map_tick(&k) == 1, "D-220: the key starts the exit fade");
            _check(map_fade_at(map_fade_starts() - 1u, &f, &e, &d, &t) &&
                       f == 0u && e == 0x100u && d == 0x23u,
                   "D-220: 1000:062B fades all 256 entries (0x23)");
            k.lastKey = 0;
            while (map_tick(&k) == 1 && guard++ < 3000u) palette_tick();
            _check(guard < 3000u, "D-220: the fade and hold end the MAP");
        }
    }


    /* 9. the last four tails and the level-end probe, field by field.
     *    These run last: they free records and re-arm the bar. */
    {
        uint8_t *spare = object_slot(5u);
        uint8_t ent[2u + 7u * 16u]; /* D-59: the +0xA entity records */

        /* the tribe counter is armed from the level start (12 members, 4 required),
         * so the probes below would take the goal form; empty it first to
         * exercise 0xF5BC's taken branch, and re-arm it for the goal test. */
        statusbar_set_tribe_digits(0u, 0u);
        _check(!statusbar_tribe_count_nonzero(),
               "an empty tribe counter is the taken branch");

        /* --- 1000:DBCD (type 28): the 0x6868 table walk.  The stepper only
         * fires when +0xA counts below zero, so each step re-seeds it. --- */
        memset(spare, 0, 0x12u);
        wr_le_u16(spare + 8u, 28u);
        wr_le_u16(spare + 4u, 0x20u);
        wr_le_u16(spare + 0xCu, 0u);
        wr_le_u16(spare + 0xAu, 0u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 4u) == 0xCDu &&
                   rd_le_u16(spare + 0xCu) == 1u &&
                   rd_le_u16(spare + 0xAu) == 9u,
               "0xDBCD: the frame takes the 0x6868 byte and the cursor steps");
        wr_le_u16(spare + 0xAu, 0u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 4u) == 0xCEu && rd_le_u16(spare + 0xCu) == 2u,
               "0xDBCD: the next byte (0x6868[1] = 0xCE)");
        /* the climb: at frame 0xD3 with y not on a row boundary */
        wr_le_u16(spare + 4u, 0xD3u);
        wr_le_u16(spare + 2u, 0x45u);
        wr_le_u16(spare + 0xAu, 0u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 2u) == 0x46u,
               "0xDBCD: at 0xD3 the record climbs a pixel");
        /* and the row boundary runs the level-end probe (0xFFFF scratch pairs,
         * a non-acting record: the taken branch) */
        wr_le_u16(spare + 2u, 0x40u);
        wr_le_u16(spare + 0xAu, 0u);
        wr_le_u16(spare + 6u, 0u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 0u) == 0xFFFFu &&
                   rd_le_u16(spare + 2u) == 0xFFFFu && spare[6] == 0xFFu &&
                   rd_le_u16(spare + 8u) == 0x1Bu,
               "0xDBCD: a row boundary runs 0xF5BC's taken branch");

        /* --- 1000:DE7E (type 30): the 0x687C {dx, dy, flag} walk.  Every flag
         * byte in the shipped table is 0x01, so the armed tribe counter probe inside
         * this tail is unreachable with the shipped data (re_map.md). --- */
        memset(spare, 0, 0x12u);
        wr_le_u16(spare + 8u, 30u);
        wr_le_u16(spare + 4u, 0x20u);
        wr_le_u16(spare + 0xCu, 0u);
        wr_le_u16(spare + 0xAu, 0u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 0xCu) == 2u && rd_le_u16(spare + 0xAu) == 6u &&
                   rd_le_u16(spare + 4u) == 0x21u,
               "0xDE7E: entry {1, 6, 1} steps the cursor, hold and frame");
        wr_le_u16(spare + 0xCu, 0x0Cu); /* the {-1, 0x32, 1} entry */
        wr_le_u16(spare + 0xAu, 0u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 0xCu) == 0x0Eu &&
                   rd_le_u16(spare + 0xAu) == 0x32u &&
                   rd_le_u16(spare + 4u) == 0x20u,
               "0xDE7E: entry {-1, 0x32, 1} walks the frame down");

        /* --- 1000:DED1 (type 34): +0xA is the ENTITY link (0x8000|index),
         * not an object link; [DI+4]/[DI+C]/[DI+8] are the entity fields. --- */
        memset(ent, 0, sizeof ent);
        ent[0] = 1u;
        ent[1] = 0u; /* one entity */
        wr_le_u16(ent + 2u + 4u, 2u); /* the entity's handler = 2   */
        wr_le_u16(ent + 2u + 0xCu, 0x77u); /* its waypoint; +0xE matches */
        entity_load_fixed(ent);
        memset(spare, 0, 0x12u);
        wr_le_u16(spare + 8u, 34u);
        wr_le_u16(spare + 0xAu, 0x8000u); /* the entity 0 link */
        wr_le_u16(spare + 0xEu, 0x77u);
        (void)object_command_run(spare, 0x01u);
        _check(spare[6] == 0x80u,
               "0xDED1: a +0xC == +0xE match on a 2/3 entity sets bit 7");
        spare[6] = 0x80u;
        (void)object_command_run(spare, 0x01u);
        _check(spare[6] == 0x80u && rd_le_u16(spare + 4u) == 0u,
               "0xDED1: with bit 7 set a 2/3 entity holds");
        /* bit 7 set and the entity outside 2..3 → the probe (taken branch) */
        wr_le_u16(ent + 2u + 4u, 5u);
        entity_load_fixed(ent);
        wr_le_u16(spare + 0x10u, 0x40u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 0u) == 0xFFFFu && spare[6] == 0xFFu,
               "0xDED1: bit 7 + a non-2/3 entity runs the probe");
        /* bit 7 clear and the entity outside 2..3 → the home form */
        memset(spare, 0, 0x12u);
        wr_le_u16(spare + 8u, 34u);
        wr_le_u16(spare + 0xAu, 0x8000u);
        wr_le_u16(spare + 4u, 0x21u);
        wr_le_u16(spare + 0xCu, 0x55u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 4u) == 0x10u && rd_le_u16(spare + 8u) == 2u &&
                   rd_le_u16(spare + 0xAu) == 0u &&
                   rd_le_u16(spare + 0xCu) == 0u,
               "0xDED1: bit 7 clear + a non-2/3 entity is the home form");

        /* --- 1000:EF66 (type 22): the 5-frame step to frame 0x2E, then the
         * probe.  With the tribe counter empty the record is taken. --- */
        memset(spare, 0, 0x12u);
        wr_le_u16(spare + 8u, 22u);
        wr_le_u16(spare + 4u, 0x2Du);
        wr_le_u16(spare + 0xAu, 0u);
        (void)object_command_run(spare, 0x01u);
        _check(rd_le_u16(spare + 4u) == 0x2Eu &&
                   rd_le_u16(spare + 0xAu) == 5u &&
                   rd_le_u16(spare + 0u) == 0xFFFFu &&
                   rd_le_u16(spare + 8u) == 0x1Bu,
               "0xEF66: the step to 0x2E runs 0xF5BC's taken branch");

        /* --- 1000:F5BC's goal branch: with the tribe counter showing a count
         * the record takes the goal form instead (frame 0x2D / type 0x21). --- */
        statusbar_set_tribe_digits(0u, 4u); /* the digits show "04" */
        _check(statusbar_tribe_count_nonzero(),
               "the tribe counter is armed for the goal form");
        wr_le_u16(ent + 2u + 4u, 5u);
        wr_le_u16(ent + 2u + 8u, 50u); /* -> [0x4222] = 50 + 0x28 */
        entity_load_fixed(ent);
        memset(spare, 0, 0x12u);
        wr_le_u16(spare + 8u, 34u);
        wr_le_u16(spare + 0xAu, 0x8000u);
        wr_le_u16(spare + 0x10u, 100u); /* -> [0x4220] */
        wr_le_u16(spare + 0xEu, 0u);
        spare[6] = 0x80u; /* bit 7: the probe path */
        (void)object_command_run(spare, 0x01u);
        _check(
            rd_le_u16(spare + 4u) == 0x2Du && rd_le_u16(spare + 8u) == 0x21u &&
                rd_le_u16(spare + 0xAu) == 5u && rd_le_u16(spare + 0u) == 92u,
            "0xF5BC: the armed tribe counter takes the goal form at [0x4220] - 8");
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

        for (i = 8u; i <= object_live_count(); i++) {
            if ((object_slot_class(i - 8u) & 0x3Fu) == 5u) {
                sp = object_slot(i);
                break;
            }
        }
        _check(sp != NULL, "level 1's spear is behaviour 5");
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
                object_update_list();
            }
            _check(
                (rd_le_u16(sp + 0)) == 356u && (rd_le_u16(sp + 4)) == 0x5Cu,
                "1000:E039 moves the spear by its +0xC and 0xCDF4 picks 0x5C");
            sp[4] = 0x58u;
            sp[5] = 0u;
            sp[12] = 0xF0u;
            sp[13] = 0xFFu;
            sp[14] = 1u;
            sp[15] = 0u;
            for (k = 0; k < 2u; k++) {
                object_tick(NULL);
                object_update_list();
            }
            _check((rd_le_u16(sp + 4)) == 0x64u,
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
                object_update_list();
            }
            _check(sp[6] == 0xFFu && object_slot_class(1u) == 0u,
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

        object_set_slot_class(1u, 4u);
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
        object_update_list();
        _check((rd_le_u16(t + 4)) == 0xCDu && (rd_le_u16(t + 8)) == 0x1Cu &&
                   (rd_le_u16(t + 10)) == 9u && p[6] == 0xFFu,
               "1000:D41B's water branch writes the drown state (D-97)");
        _check(p[16] == 0x77u && p[17] == 0x77u,
               "1000:D4D4's ADD SP,2/RET leaves 1000:CEC3 (D-176)");
    }
    if (level_load(0) == 0) {
        uint8_t *p = object_slot(9), *t = object_slot(0);

        object_set_slot_class(1u, 4u);
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
        object_update_list();
        _check((rd_le_u16(t + 4)) == 0x50u,
               "1000:D162 dispatches the 0x666B hit op (D-99)");
    }

    /* D-104/D-144: the class block is 0x3E5E..0x3E72 (21 bytes); the terrain
     * walk clears 0x3E5F..0x3E72 only. */
    {
        static uint8_t flat[640];
        static uint8_t tbl[0x25F];

        object_init();
        object_set_slot_class(0u, 0xAAu);
        object_set_slot_class(20u, 0x5Au);
        (void)object_load_terrain_objects(flat);
        _check(object_slot_class(0u) == 0xAAu && object_slot_class(20u) == 0u,
               "1000:5175 clears 0x3E5F..0x3E72, not 0x3E5E (D-104)");
        object_set_slot_class(0u, 0u);
        object_set_slot_class(20u, 0x5Au);
        object_save_table(tbl);
        _check(tbl[0x3Eu + 20u] == 0x5Au,
               "the save carries the class byte at 0x3E72 (D-144)");
        object_set_slot_class(20u, 0u);
        object_load_table(tbl);
        _check(object_slot_class(20u) == 0x5Au,
               "the restore loads the class byte at 0x3E72");
    }

    /* D-86: the charged leap.  A full-charge release must travel; the three
     * landing probes of `objFallMachine` read the cell one row below in the
     * SAME column (1000:F7D7/F813/F86F move the row word `CX`, not the probe
     * x), the item mask shifts by the item probe's own sub-column
     * (1000:F90B/F950), and the feet-cell "want" byte of the leap types
     * 0x19/0x1A is the level's field-K second word (1000:4E99 writes
     * [0x6F0B]), not the image's pre-load bytes. */
    if (level_load(15) == 0) { /* level 16, BAD TASTE */
        uint32_t minx, apex, ty, fr;

        _check(level_special_tile(2u) == 0x86u &&
                   level_special_tile(3u) == 0x83u,
               "level 16's field-K second word is the 0x6F0B pair (1000:4E99)");
        /* D-85's live trace: (467,168) -> (411,168), apex y 155. */
        _tf_leap(467, 168, &minx, &apex, &ty, &fr);
        _check(minx == 411u,
               "the full-charge leap reaches x = 411 (D-85 live)");
        _check(apex == 155u, "the leap's apex is y = 155 (D-85 live)");
        _check(ty == 1u && fr == 8u,
               "the leap lands standing, frame 8 / type 1 (D-85 live)");
        /* the water hole: row 19's tiles F0/F1/F2 span x 320..367 and the
         * walkable rim has a 16-px opening at x 336..351, so a leftward leap
         * from the right rim must clear the water and land on the far side.
         * A record standing at x = 366 sits at y = 264 (the rim's surface,
         * 288, minus the 0x18 of 1000:F75B's feet line). */
        _tf_leap(366, 264, &minx, &apex, &ty, &fr);
        _check(minx <= 320u,
               "the leap crosses level 16's water hole (x <= 320, D-86)");
        _check(ty == 1u, "the water-hole leap lands standing, not drowning");
    }

    /* 6c6. D-260 (closed 2026-11-09): the wheel's wall bounce stops at the
     * wall.  PARKVIEW (level 26) has no 0xAE tile; the witchdoctor's op-10
     * claim (program 5, recipe 3 = sprite 0x38/behaviour 4) turns an adjacent
     * human into the wheel (1000:3A6A).  Riding it right into the row-17/18
     * wall must stop it (1000:D3E3 zeroes a bounce that does not cross the
     * sign); the old positive-vx arm kept 0x20-vx and rolled it off the ledge,
     * killing the rider.  A human walking the same floor stops at x=133, so
     * the wall's item mask is real and the difference is the bounce arm. */
    if (level_load(25) == 0) {
        int32_t player = object_player();
        int32_t nb = player >= 0 ? object_find_neighbour(player) : -1;
        uint32_t wheel = 0u, t;

        _check(player >= 0, "PARKVIEW has the 0xC0 player record");
        _check(nb >= 0, "PARKVIEW's witchdoctor has a neighbour to claim");
        level_begin();
        for (t = 0; t < 4000u && level_starting(); t++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        /* the claim runs after the start chain: level_spark_begin needs the
         * composed level page */
        if (player >= 0 && nb >= 0 &&
            object_claim_begin(3u, player) == nb) {
            t = 0u;
            while (object_claim_active() && t++ < 5000u) {
                level_tick();
                palette_tick();
                if (level_loading_screen()) level_loading_tick();
            }
            _check(!object_claim_active(), "the PARKVIEW claim finishes");
            for (t = 1u; t <= 20u; t++)
                if (object_slot_class(t) == 4u) {
                    wheel = t;
                    break;
                }
            _check(wheel != 0u, "the claim appends the behaviour-4 wheel");
        }
        if (wheel != 0u) {
            uint8_t *it = object_slot(8u + wheel);
            uint8_t *actor = object_slot((uint32_t)player);
            uint32_t ix = rd_le_u16(it + 0u), iy = rd_le_u16(it + 2u);

            _check(rd_le_u16(it + 4u) == 0x0038u,
                   "the claim's wheel is frame 0x38 (recipe 3)");
            _tf_wr16(actor, 0u, ix - 8u);
            _tf_wr16(actor, 2u, iy - 0x10u);
            _tf_wr16(actor, 4u, 0x0010u);
            actor[6] = 0u;
            _tf_wr16(actor, 8u, 2u);
            _tf_wr16(actor, 0xAu, 0u);
            object_set_action_entity((uint32_t)player);
            object_action_set_cursor(1u);
            object_action_set_program(1u);
            object_set_action_latched(0);
            object_action_run(); /* the give / hold */
            _check(rd_le_u16(actor + 8u) == 0x000Fu &&
                       object_slot_class(wheel) == 0x84u,
                   "the PARKVIEW wheel mounts");
            for (t = 0; t < 200u; t++) {
                (void)object_command_run(actor, 0x08u); /* right */
                object_update_list();
            }
            _check((it[6] & 0x80u) == 0u,
                   "D-260: the wheel is not freed at the wall");
            _check(rd_le_u16(it + 8u) == 0u,
                   "D-260: the wall bounce zeroes vx (1000:D3EE)");
            _check(rd_le_u16(it + 0u) <= 0x85u,
                   "D-260: the wheel stops before the wall corner (x <= 133)");
            _check((uint32_t)rd_le_u16(actor + 0u) + 8u ==
                       (uint32_t)rd_le_u16(it + 0u) &&
                       rd_le_u16(actor + 8u) == 0x000Fu,
                   "D-260: the rider stays mounted at the wall");
        }
    }

    if (_bad) {
        fprintf(stderr, "flow: FAIL\n");
        return 1;
    }
    printf("flow: ok (the level gate, the post-level spine, the end page)\n");
    return 0;
}
