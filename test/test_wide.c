/* test_wide.c - the optional doubled level viewport (--view wide).
 *
 * Part 1 needs no data: the scale switch and the 2x expand of the logical
 * screen into the display page.
 * Part 2 needs the volumes: level 0's start chain in wide mode - the
 * 640x400 page, the world's 64-pixel left margin, the bar band at rows
 * 304..383 doubled from its 320x40 art, the loading screen's bar inside the
 * 320x200 logical screen, and the camera clamps.
 *
 * argv[1] = the volume dir (HUMANS.00N).  Missing volumes = skip.
 */
#include "audio.h"
#include "compat.h"
#include "font.h"
#include "frontend.h"
#include "level/level.h"
#include "null/osys_null.h"
#include "object/object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "scene/scene.h"
#include "score.h"
#include "statusbar.h"
#include "textpage.h"
#include "video.h"
#include "test_support.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Whether a screen pixel's 2x2 block holds v.
 *
 * @param page the display page
 * @param w the page width
 * @param x the screen x
 * @param y the screen y
 * @param v the value
 * @return 1 when all four page pixels hold v, 0 otherwise
 */
static int32_t _block_is(const uint8_t *page, uint32_t w, uint32_t x,
                         uint32_t y, uint8_t v)
{
    return page[(2u * y) * w + 2u * x] == v &&
           page[(2u * y) * w + 2u * x + 1u] == v &&
           page[(2u * y + 1u) * w + 2u * x] == v &&
           page[(2u * y + 1u) * w + 2u * x + 1u] == v;
}

/**
 * @brief Write the display page and the working palette as a binary PPM.
 *
 * @param path the file path
 * @param page the display page
 * @param w the page width
 * @param h the page height
 */
static void _dump_ppm(const char *path, const uint8_t *page, uint32_t w,
                      uint32_t h)
{
    const uint8_t *pal = osys_null_palette(); // the live DAC
    FILE *f = fopen(path, "wb");
    uint32_t i, c;

    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    for (i = 0; i < w * h; i++) {
        uint32_t idx = page[i];

        for (c = 0; c < 3u; c++) {
            uint8_t v6 = pal[idx * 3u + c];
            fputc((uint8_t)((v6 << 2) | (v6 >> 4)), f);
        }
    }
    fclose(f);
    printf("wide: wrote %s\n", path);
}

/**
 * @brief Part 1: the scale switch and the expand, with no game data.
 */
static void _metrics_and_expand(void)
{
    uint8_t *page;
    uint8_t *scr;
    uint32_t x, y;

    osys_null_set_viewport(VIEWPORT_WIDE);
    video_init();
    if (video_viewport_mode() != VIEWPORT_WIDE || video_page_w() != 640u ||
        video_page_h() != 400u)
        TEST_FAILF("wide: page is %ux%u (view %d)", (unsigned)video_page_w(),
                   (unsigned)video_page_h(), (int)video_viewport_mode());
    /* fetch page 0 first: handing it out marks the display page as the
     * frame's surface, and video_screen() then marks the logical screen */
    page = video_page(0);
    scr = video_screen();
    if (page == scr) TEST_FAILF("wide: the logical screen is page 0");
    for (y = 0; y < VIDEO_SCREEN_H; y++)
        for (x = 0; x < VIDEO_SCREEN_W; x++)
            scr[y * VIDEO_SCREEN_W + x] = (uint8_t)(x * 3u + y * 5u + 7u);
    video_present_frame();
    for (y = 0; y < VIDEO_SCREEN_H; y++)
        for (x = 0; x < VIDEO_SCREEN_W; x++) {
            uint8_t v = (uint8_t)(x * 3u + y * 5u + 7u);

            if (!_block_is(page, video_page_w(), x, y, v))
                TEST_FAILF("wide: expand differs at (%u,%u)", (unsigned)x,
                           (unsigned)y);
        }
    /* a present with no screen drawing must not re-expand over the page */
    page[0] = 0x5A;
    video_present_frame();
    if (page[0] != 0x5A)
        TEST_FAILF("wide: an idle frame re-expanded the screen");

    /* classic: the same accessors, and the screen is page 0 itself */
    osys_null_set_viewport(VIEWPORT_CLASSIC);
    video_init();
    if (video_viewport_mode() != VIEWPORT_CLASSIC ||
        video_page_w() != VIDEO_SCREEN_W || video_page_h() != VIDEO_SCREEN_H ||
        video_screen() != osys_null_page(0))
        TEST_FAILF("classic: the metrics or the screen changed");
}

/**
 * @brief Part 2: level 0's start chain and play frame in wide mode.
 */
static void _level_wide(const char *dump_loading)
{
    const uint8_t *page;
    uint32_t w = video_page_w(), h = video_page_h();
    uint32_t t, x, y;
    uint8_t saw_loading = 0;
    uint8_t clean_checked = 0;

    level_begin();
    for (t = 0; t < 4000u && level_starting(); t++) {
        level_tick();
        palette_tick();
        audio_tick();
        if (!level_loading_screen()) continue;
        saw_loading = 1;
        level_loading_tick();
        /* the bar slides up over the first 48 display periods; check the
         * settled frame (the loading screen runs 0xA0 + 0x56 of them) */
        if (statusbar_split() != 152u) continue;
        /* the loading screen is the 320x200 logical screen: the bar is at
         * rows 152..191 at 1x, and the present doubles the whole screen */
        {
            static uint8_t shot[VIDEO_SCREEN_BYTES];
            const uint8_t *scr = video_screen();
            uint32_t n = 0;

            if (scr == osys_null_page(0))
                TEST_FAILF("wide: the loading screen is the page");
            memcpy(shot, scr, sizeof shot);
            /* scale 2: the bar's 1x rows move to the bottom of the screen
             * (160..199) so it lines up with the level's own bar */
            for (y = 160u; y < 200u; y++)
                for (x = 0; x < VIDEO_SCREEN_W; x++)
                    if (shot[y * VIDEO_SCREEN_W + x] < 0xC0u) n++;
            if (n)
                TEST_FAILF("wide: the loading bar is not at rows "
                           "160..199 (%u pixels)",
                           (unsigned)n);
            /* the screen is drawn at 320 wide, not sheared into the page:
             * on the first settled frame (the walkers phase, whose lowest
             * sprite row is 60) the rows above them are clean background */
            if (!clean_checked) {
                clean_checked = 1;
                n = 0;
                for (y = 0; y < 57u; y++)
                    for (x = 0; x < VIDEO_SCREEN_W; x++)
                        if (shot[y * VIDEO_SCREEN_W + x]) n++;
                if (n)
                    TEST_FAILF("wide: the loading screen is not clean "
                               "above the sprite band (%u pixels)",
                               (unsigned)n);
            }
            video_present_frame();
            page = osys_null_page(0);
            if (dump_loading) _dump_ppm(dump_loading, page, w, h);
            for (y = 160u; y < 200u; y++)
                for (x = 0; x < VIDEO_SCREEN_W; x++)
                    if (!_block_is(page, w, x, y, shot[y * VIDEO_SCREEN_W + x]))
                        TEST_FAILF("wide: the present did not double "
                                   "the loading screen at (%u,%u)",
                                   (unsigned)x, (unsigned)y);
        }
    }
    if (!saw_loading) TEST_FAILF("wide: the loading screen never showed");
    if (level_starting()) TEST_FAILF("wide: the start chain did not finish");
    /* the seed is clamped to the empty pan range, so the map never starts
     * off-centre */
    if (level_view_x() != 0u || level_view_y() != 0u)
        TEST_FAILF("wide: the view starts at (%u,%u)", (unsigned)level_view_x(),
                   (unsigned)level_view_y());
    for (t = 0; t < 40u; t++) { // settle the camera onto the actor
        level_tick();
        palette_tick();
        audio_tick();
    }

    page = osys_null_page(0);
    if (w != 640u || h != 400u)
        TEST_FAILF("wide: the play page is %ux%u", (unsigned)w, (unsigned)h);
    /* the bar band: rows 320..399, every pixel carries the +0xC0 tag */
    for (y = 320u; y < 400u; y++)
        for (x = 0; x < w; x++)
            if (page[y * w + x] < 0xC0u)
                TEST_FAILF("wide: the bar is not at row %u x %u (%02X)",
                           (unsigned)y, (unsigned)x, page[y * w + x]);
    /* the left margin of the viewport: black, because the 512-wide world is
     * copied at x 64 and nothing is drawn left of it */
    for (y = 0; y < 320u; y++)
        for (x = 0; x < 64u; x++)
            if (page[y * w + x] != 0u)
                TEST_FAILF("wide: the left margin is not black at "
                           "(%u,%u)",
                           (unsigned)x, (unsigned)y);
    /* the world is actually drawn into the viewport */
    {
        uint32_t ink = 0;

        for (y = 0; y < 320u; y++)
            for (x = 64u; x < 576u; x++)
                if (page[y * w + x]) ink++;
        if (ink < 1000u)
            TEST_FAILF("wide: the viewport is nearly empty (%u)",
                       (unsigned)ink);
    }
    /* the in-level dialog: rendered at 1x and copied at 2x, so every ink
     * pixel belongs to a uniform 2x2 block and the box starts on even
     * coordinates, centred on the 640-wide page */
    {
        static uint8_t pg[VIDEO_PAGE_BYTES];
        uint32_t tiles = font_text_width("HELLO") / 16u + 2u;
        uint32_t cols = tiles + 2u, lines = 1u + 2u;
        uint32_t left = 160u - cols * 8u, top = 76u - lines * 8u;

        memset(pg, 0, sizeof pg);
        textpage_draw_level(pg, w, 2u, "HELLO", -1, 0u, 0);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint8_t v = pg[y * w + x];

                if (!v) continue;
                if (pg[(y & ~1u) * w + (x & ~1u)] != v ||
                    pg[(y & ~1u) * w + (x & ~1u) + 1u] != v ||
                    pg[((y & ~1u) + 1u) * w + (x & ~1u)] != v)
                    TEST_FAILF("wide: the dialog is not 2x at (%u,%u)",
                               (unsigned)x, (unsigned)y);
            }
        if (pg[(2u * top) * w + 2u * left] == 0u ||
            (2u * left > 0u && pg[(2u * top) * w + 2u * left - 1u] != 0u))
            TEST_FAILF("wide: the dialog is not centred at 2x");
    }
    /* the field the backend is told to scan out: the level's whole page,
     * not the mode's 192*2 = 384 (which would cut the bar's bottom) */
    video_present_frame();
    if (osys_null_field_rows() != 400u)
        TEST_FAILF("wide: the level's field is %u rows",
                   (unsigned)osys_null_field_rows());
    /* the pan is forced: the viewport is exactly the 320-row world, so the
     * camera sits at (0,0) and the map never moves */
    level_follow_view();
    if (level_view_x() != 0u || level_view_y() != 0u)
        TEST_FAILF("wide: the camera is at (%u,%u)", (unsigned)level_view_x(),
                   (unsigned)level_view_y());
}

/**
 * @brief Run the wide-viewport checks.
 */
static void test_wide(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : ".";
    char path[512];
    FILE *f;

    osys_bind(&g_osys_null);
    _metrics_and_expand();

    snprintf(path, sizeof path, "%s/HUMANS.001", vol_dir);
    f = fopen(path, "rb");
    if (!f) TEST_SKIPF("skip: no volume files at %s (part 2)", vol_dir);
    fclose(f);
    if (compat_chdir(vol_dir)) TEST_FAILF("chdir %s", vol_dir);

    osys_null_set_viewport(VIEWPORT_WIDE);
    res_init();
    video_init();
    palette_init();
    scene_init();
    frontend_init();
    audio_init();
    level_init();
    level_seed_tribe(); // entry 1000:00B1's 12/10/8 members
    statusbar_init();
    if (level_load(0) != 0) TEST_SKIPF("skip: no level data");
    _level_wide(_test_argc > 3 ? _test_argv[3] : NULL);
    if (_test_argc > 2)
        _dump_ppm(_test_argv[2], osys_null_page(0), video_page_w(),
                  video_page_h());

    /* leaving the level for a mode-X screen that is not the level (the
     * front-end room): the field goes back to the mode's 192*2 = 384 rows,
     * so the room's never-shown rows 192..199 are not scanned out */
    {
        InputState idle;
        uint8_t *scr;
        uint32_t i;

        memset(&idle, 0, sizeof idle);
        /* the room loads on the first tick and fades in over the next ones */
        for (i = 0; i < 400u; i++) {
            frontend_tick(&idle);
            palette_tick();
            audio_tick(); // the room's load waits for its music fade
        }
        scr = video_screen();
        video_present_frame();
        if (scr == osys_null_page(0) || osys_null_field_rows() != 384u)
            TEST_FAILF("wide: the room's field is %u rows",
                       (unsigned)osys_null_field_rows());
        if (_test_argc > 4)
            _dump_ppm(_test_argv[4], osys_null_page(0), video_page_w(), 384u);
    }

    /* the witchdoctor's claim on PARKVIEW (level 26): the pan frames draw
     * the two records on the recoloured black page.  The 512-wide world is
     * copied at x = 64, so the records must land at world x + 64; the port
     * used to omit the margin here (the player and the victim sit at the
     * map's bottom left, so the animation appeared in the page's corner).
     * After 1000:3D5E every page byte is 0x1E (the recoloured world) or the
     * untouched >= 0x80 banks, so any other byte is record ink. */
    {
        int32_t player = -1, nb = -1;
        uint32_t t, x, y;

        if (level_load(25) != 0) TEST_FAILF("wide: PARKVIEW failed to load");
        player = object_core_player();
        nb = player >= 0 ? object_sequence_find_neighbour(player) : -1;
        if (player < 0 || nb < 0)
            TEST_FAILF("wide: PARKVIEW has no witchdoctor claim");
        level_begin();
        for (t = 0; t < 4000u && level_starting(); t++) {
            level_tick();
            palette_tick();
            audio_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        if (object_sequence_claim_begin(3u, (uint32_t)player) != nb)
            TEST_FAILF("wide: the witchdoctor's claim did not start");
        for (t = 0; t < 600u && object_sequence_claim_active(); t++) {
            level_tick();
            palette_tick();
            audio_tick();
            if (!object_sequence_claim_pan_active()) continue;
            {
                const uint8_t *pg = osys_null_page(0);
                uint32_t ink_in_margin = 0u, ink_right = 0u;

                if (_test_argc > 5)
                    _dump_ppm(_test_argv[5], pg, video_page_w(),
                              video_page_h());

                for (y = 0; y < video_page_h(); y++)
                    for (x = 0; x < video_page_w(); x++) {
                        uint8_t v = pg[y * video_page_w() + x];

                        if (v == 0x1Eu || v >= 0x80u) continue;
                        if (x < 64u) ink_in_margin++;
                        else if (x < 576u) ink_right++;
                    }
                if (ink_in_margin)
                    TEST_FAILF("wide: the claim's records are drawn in "
                               "the left margin (%u pixels)",
                               (unsigned)ink_in_margin);
                if (!ink_right)
                    TEST_FAILF("wide: the claim's records are not at the "
                               "world x + the margin");
            }
            break;
        }
    }

    /* the score screen after a won level: the 320x200 card is drawn over the
     * level's frozen page and its zero bytes keep the level.  In the doubled
     * view the page's world sits at the top of the field (the level's own
     * framing, the bar parked), so the score centres it: rows 0..319 move
     * down to 40..359 and the padding is cleared.  SCORE.BIT's rows 0 and 60
     * are fully transparent, so the card reveals exactly that centred page. */
    {
        static uint8_t pre[VIDEO_PAGE_BYTES];
        const uint8_t *pg = osys_null_page(0);
        uint32_t w = video_page_w(), x, y, changed = 0, witness = 0;
        uint32_t pad = (video_page_h() - 320u) / 2u; // the centring slack
        uint32_t tr = 60u;                           // a transparent card row

        /* the end flow's state: draw_level_text parks the bar and re-renders
         * the frozen world page */
        statusbar_hide();
        level_end_redraw_page();
        memcpy(pre, pg, sizeof pre);
        /* the shift must be observable: the same columns at rows 80 and 120
         * are different map rows */
        for (x = 0; x < w; x++)
            if (pre[80u * w + x] != pre[120u * w + x]) witness++;
        if (!witness || pad != 40u)
            TEST_FAILF("wide: no witness for the score centring");
        if (score_begin(1234u) != 1)
            TEST_FAILF("wide: the score screen did not start");
        video_present_frame(); // the flow loop's per-frame present
        while (palette_fade_active()) palette_tick(); // the ISR steps the fade
        if (_test_argc > 6) _dump_ppm(_test_argv[6], pg, w, video_page_h());
        /* the card's transparent top rows show the centring's padding */
        for (x = 0; x < w; x++)
            if (pg[0u * w + x] || pg[1u * w + x])
                TEST_FAILF("wide: the score's top padding is not black "
                           "at x=%u",
                           (unsigned)x);
        /* a transparent mid-card row shows the world 40 rows lower */
        for (x = 0; x < w; x++) {
            if (pg[(2u * tr) * w + x] != pre[(2u * tr - pad) * w + x])
                TEST_FAILF("wide: the score's background is not centred "
                           "at x=%u",
                           (unsigned)x);
            if (pg[(2u * tr + 1u) * w + x] != pre[(2u * tr + 1u - pad) * w + x])
                TEST_FAILF("wide: the score's background is not centred "
                           "(odd row) at x=%u",
                           (unsigned)x);
        }
        for (y = 0; y < video_page_h(); y++)
            for (x = 0; x < w; x++)
                if (pg[y * w + x] != pre[y * w + x]) changed++;
        if (changed < 1000u)
            TEST_FAILF("wide: the score card was not drawn over the "
                       "level (%u pixels)",
                       (unsigned)changed);
    }
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_wide);
    return UNITY_END();
}
