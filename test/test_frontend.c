/* test_frontend.c - the front-end room's option read side (the ACTION key).
 *
 * Drives frontend_tick with synthetic input: walks the rails to each option
 * cell, presses ACTION (joyBits bit 7), and asserts:
 *   (a) the counter advances and wraps at the cell's count;
 *   (b) the composed page's badge pixels are the next shipped sprite cell
 *       (byte-checked against CODEB1/CODEB2/RESTORE, the files the placer
 *       reads: a cell's opaque pixels must equal the page's);
 *   (b2) a synthetic transparent cell: nonzero pixels are placed, zero
 *       pixels leave the page untouched (the _page_cells rule);
 *   (c) the value reaches its consumer - LEVEL -> the record's time B and
 *       the 10-member tribe (entry 1000:009F / render_level_frame 1000:958C);
 *       MUSIC -> the frame web's gate (audio_music_gate 1000:A5E8) stopping
 *       and resuming the backend; ANIMATIONS -> scene_play_level_intro's
 *       early RET (cd_file_op 1000:AFFD); RESTORE GAME -> carried out of the
 *       room (its reader, entry 1000:004C, is the M6 save load).
 *
 * The cells and counts are 202F:0x5E46 (cell 2 = LEVEL/3, 3 = MUSIC/2,
 * 5 = ANIMATIONS/2, 6 = RESTORE GAME/21) and the signs are painted in
 * code2.raw.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * Missing volumes = skip. */
#include "audio.h"
#include "font.h"
#include "frontend.h"
#include "level/level.h"
#include "null/osys_null.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "scene/scene.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"
#include "test_support.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Run one frontend tick and its palette ISR step.
 *
 * @param in the input state
 */
static void _tick(const InputState *in)
{
    frontend_tick(in);
    palette_tick();                 // game_tick's ISR body
}

/**
 * @brief A zeroed input state (no key, fire released).
 *
 * @return the zeroed input state
 */
static InputState _idle(void)
{
    InputState in;
    memset(&in, 0, sizeof in);
    return in;
}

/** @brief Hold `dirBit` until the rail arrival changes the cell; one _idle _tick is
 * left afterwards so the rail cannot re-arm from the held key.
 *
 * @param dirBit the direction bit
 * @param want the wanted value
 * @param what the check description
 * @return 0 when the target was reached, 1 otherwise
 */
static int32_t _walk_to(uint32_t dirBit, uint32_t want, const char *what)
{
    uint32_t start = frontend_cell_for_probe();
    InputState in = _idle();
    in.joyBits = dirBit;
    for (uint32_t i = 0; i < 400u; i++) {
        _tick(&in);
        if (frontend_cell_for_probe() != start) break;
    }
    if (frontend_cell_for_probe() != want) {
        fprintf(stderr, "FAIL walk %s: cell %u, want %u\n", what,
                frontend_cell_for_probe(), want);
        return 1;
    }
    InputState rel = _idle();
    _tick(&rel);
    return 0;
}

/** @brief One ACTION press + release (the original spins on the held key; the port
 * latches one cycle per press).  The release _tick is the next frame web's
 * pass, so the music gate sees the new counter there.
 */
static void _press_action(void)
{
    InputState in = _idle();
    in.joyBits = JOYBITS_FIRE;
    _tick(&in);
    InputState rel = _idle();
    _tick(&rel);
}

/** @brief (b): the shipped cell's opaque pixels must be on the composed page at the
 * badge's place - skipping the 16x16 block the walker's own sprite overlays
 * (room_draw_character draws him in front of the sign; the sign's copy in
 * code2.raw is patched too, so the rest of his 27x20 backdrop box must still
 * carry the new badge).  set_scroll is 0x140 at all four cells this test
 * visits (the upper walkway y < 0x64 gives 0x140, and the floor targets
 * 466/520 clamp to it), so world x = col*4 maps to page x = col*4-0x140.
 *
 * @param what the check description
 * @param dest the destination
 * @param spr the sprite index
 * @param size the size
 * @param srcOff the source offset
 * @param w the width
 * @param h the height
 * @param cell the map cell
 * @param charX the character x
 * @param charY the character y
 * @return 0 when the check passes, 1 otherwise
 */
static int32_t _check_badge(const char *what, uint32_t dest, const uint8_t *spr,
                            uint32_t size, uint32_t srcOff, uint32_t w,
                            uint32_t h, uint32_t cell, uint32_t charX,
                            uint32_t charY)
{
    const uint8_t *page = osys_null_page(0);
    uint32_t off = srcOff + cell * w * h;
    uint32_t row = dest / 160u, col = dest % 160u;
    uint32_t sx = col * 4u - 0x140u;
    uint32_t checked = 0;

    if (!spr || size < off + w * h || row + h > 200u || col * 4u < 0x140u ||
        sx + w > 320u) {
        fprintf(stderr, "FAIL badge %s: geometry out of range\n", what);
        return 1;
    }
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++) {
            uint8_t v = spr[off + r * w + c];
            uint32_t wx = col * 4u + c, wy = row + r;
            if (!v) continue;              // nonzero-transparent
            if (wx >= charX + 2u && wx <= charX + 17u && wy >= charY &&
                wy <= charY + 23u)
                continue;   // the walker
            checked++;
            if (page[wy * 320u + wx - 0x140u] != v) {
                fprintf(stderr,
                        "FAIL badge %s: cell %u px (%u,%u) = %u, "
                        "want %u\n",
                        what, cell, c, r, page[wy * 320u + wx - 0x140u], v);
                return 1;
            }
        }
    if (checked < 100u) {
        fprintf(stderr, "FAIL badge %s: only %u pixels checked\n", what,
                checked);
        return 1;
    }
    return 0;
}

/**
 * @brief Run the frontend room tests.
 */
static void test_frontend(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : ".";
    char path[512];

    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        TEST_SKIPF("skip: no volume files at %s (fixture data absent)",
                   vol_dir);
    fclose(f);
    if (compat_chdir(vol_dir)) TEST_FAILF("chdir %s failed", vol_dir);

    osys_bind(&g_osys_null);
    res_init();
    video_init();
    palette_init();
    font_init();
    audio_init();
    statusbar_init();
    level_init();
    scene_init();
    frontend_init();

    /* The transparent cell: _page_cells writes nonzero pixels only, and the
     * flat 4*addr + p mapping must put them on the same page pixels the
     * plane model did.  Row 0 src {1,0,2} -> x1=1, x2 keeps 0x5A, x3=2;
     * row 1 src {0,3,0} -> x1/x3 keep, x2=3. */
    {
        static const uint8_t cell[6] = { 1u, 0u, 2u, 0u, 3u, 0u };
        uint8_t *row0 = video_vram_group(0x3000u);
        uint8_t *row1 = video_vram_group(0x30A0u);
        int32_t tbad = 0;

        for (uint16_t i = 0; i < 4u; i++) row0[i] = row1[i] = 0x5Au;
        frontend_page_cells_for_probe(0x3000u, cell, 3u, 3u, 2u, 1u);
        if (row0[0] != 0x5Au || row0[1] != 1u || row0[2] != 0x5Au ||
            row0[3] != 2u)
            tbad = 1;
        if (row1[0] != 0x5Au || row1[1] != 0x5Au || row1[2] != 3u ||
            row1[3] != 0x5Au)
            tbad = 1;
        if (tbad)
            TEST_FAILF("transparent cell: row0 %02x %02x %02x %02x, "
                       "row1 %02x %02x %02x %02x",
                       row0[0], row0[1], row0[2], row0[3], row1[0], row1[1],
                       row1[2], row1[3]);
    }

    /* the shipped badge art (the same chunks the placer reads, from disc 0) */
    uint32_t cb1Size = 0, cb2Size = 0, rstSize = 0;
    uint8_t *cb1 = res_load_chunk("APL\\CODEB1.SPR", &cb1Size, NULL);
    uint8_t *cb2 = res_load_chunk("APL\\CODEB2.SPR", &cb2Size, NULL);
    uint8_t *rst = res_load_chunk("APL\\RESTORE.SPR", &rstSize, NULL);
    if (!cb1 || !cb2 || !rst || cb1Size != 4u * 48u * 32u ||
        cb2Size != 3u * 80u * 32u || rstSize != 21u * 48u * 32u)
        TEST_FAILF("badge assets unreadable");

    /* (c) ANIMATIONS is ON by default: the cutscene starts.  (Done before
     * the room so the value is still 0; the scene then stays mid-play and
     * the post-room call below is a pure gate check.) */
    if (frontend_option(FRONTEND_OPT_ANIM) != 0u ||
        scene_play_level_intro(0) != 1)
        TEST_FAILF("the level intro did not start with ANIMATIONS on");
    res_set_disc(0u);           // the room's assets ship on disc 0

    /* enter the room: load + the fade-in (~24 ticks), then _idle */
    {
        InputState in = _idle();
        for (uint32_t i = 0; i < 60u; i++) _tick(&in);
    }
    if (osys_null_audio_starts() != 1u || osys_null_audio_stops() != 0u)
        TEST_FAILF("MUSIC on should resume once (starts %u, stops %u)",
                   osys_null_audio_starts(), osys_null_audio_stops());
    if (osys_null_video_mode() != VIDEO_MODE_X)
        TEST_FAILF("the room did not set the mode-X field");
    /* The backdrop is the original's 200-row copy (room_backdrop_build
     * 1000:AF07, CX=0xC8): rows 192..199 are the chunk's palette tail, and
     * reset_display_mode's 320x192 field (1000:82C6, called at 1000:A544)
     * ends on row 191, so they are written and never shown - not blanked. */
    {
        uint32_t lit = 0;

        for (uint32_t pg = 0; pg < 2u; pg++) {
            const uint8_t *q = osys_null_page((int32_t)pg);

            for (uint32_t r = 192u; r < 200u; r++)
                for (uint32_t c = 0; c < 320u; c++)
                    if (q[r * 320u + c]) lit++;
        }
        if (!lit) TEST_FAILF("the backdrop's rows past the image are blanked");
    }

    /* --- LEVEL: cell 2 (EASY/NORMAL/HARD = 3 states) ------------------- */
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_UP, 1u, "up to the walkway"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_LEFT, 2u, "left to LEVEL"));
    if (frontend_option(FRONTEND_OPT_LEVEL) != 0u)
        TEST_FAILF("LEVEL starts at %u, want 0",
                   frontend_option(FRONTEND_OPT_LEVEL));
    _press_action();
    if (frontend_option(FRONTEND_OPT_LEVEL) != 1u)
        TEST_FAILF("LEVEL did not advance to 1");
    /* the LEVEL badge: codeb2, dest 0x1C80, 80x32, cell 1 (NORMAL) */
    TEST_ASSERT_FALSE(_check_badge("LEVEL", 0x1C80u, cb2, cb2Size, 0u, 80u, 32u,
                                   1u, 400u, 55u));
    _press_action();
    _press_action();                     // 2 then the wrap to 0
    if (frontend_option(FRONTEND_OPT_LEVEL) != 0u)
        TEST_FAILF("LEVEL did not wrap at 3");
    _press_action(); // leave it on NORMAL for the level

    /* --- MUSIC: cell 3 (ON/OFF = 2 states) ----------------------------- */
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_LEFT, 3u, "left to MUSIC"));
    _press_action(); // 0 -> 1: OFF
    if (frontend_option(FRONTEND_OPT_MUSIC) != 1u)
        TEST_FAILF("MUSIC did not advance to 1");
    if (osys_null_audio_stops() != 1u || osys_null_audio_starts() != 1u)
        TEST_FAILF("MUSIC off did not stop the backend (starts %u, stops %u)",
                   osys_null_audio_starts(), osys_null_audio_stops());
    /* the MUSIC badge: codeb1 cell 1 (OFF), dest 0x1D14, 48x32 */
    TEST_ASSERT_FALSE(_check_badge("MUSIC", 0x1D14u, cb1, cb1Size, 0u, 48u, 32u,
                                   1u, 362u, 55u));
    _press_action(); // 1 -> 0: the wrap; resume
    if (frontend_option(FRONTEND_OPT_MUSIC) != 0u ||
        osys_null_audio_starts() != 2u)
        TEST_FAILF("MUSIC did not wrap/resume (counter %u, starts %u)",
                   frontend_option(FRONTEND_OPT_MUSIC),
                   osys_null_audio_starts());
    {
        InputState in = _idle();
        for (uint32_t i = 0; i < 5u; i++) _tick(&in); // the latch holds
    }
    if (osys_null_audio_starts() != 2u || osys_null_audio_stops() != 1u)
        TEST_FAILF("the music latch re-called the backend");

    /* --- on to RESTORE GAME: cell 6 (OFF/1..20 = 21 states) ----------- */
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_RIGHT, 2u, "right back to LEVEL"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_RIGHT, 1u, "right to the floor rail"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_DOWN, 0u, "down to the floor"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_RIGHT, 6u, "right to RESTORE GAME"));
    _press_action(); // 0 -> 1
    if (frontend_option(FRONTEND_OPT_GAME) != 1u)
        TEST_FAILF("RESTORE GAME did not advance to 1");
    /* the RESTORE GAME badge: restore.spr cell 1 (GAME 1), dest 0x5BBC */
    TEST_ASSERT_FALSE(_check_badge("RESTORE GAME", 0x5BBCu, rst, rstSize, 0u,
                                   48u, 32u, 1u, 520u, 160u));
    for (uint32_t i = 0; i < 20u; i++) _press_action(); // 2..20, then wrap
    if (frontend_option(FRONTEND_OPT_GAME) != 0u)
        TEST_FAILF("RESTORE GAME did not wrap at 21");
    _press_action(); // leave GAME 1 for the exit

    /* --- ANIMATIONS: cell 5 (ON/OFF = 2 states) ----------------------- */
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_LEFT, 0u, "left to the floor start"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_UP, 1u, "up to the walkway"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_UP, 4u, "up to the top rail"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_RIGHT, 5u, "right to ANIMATIONS"));
    _press_action(); // 0 -> 1: OFF
    if (frontend_option(FRONTEND_OPT_ANIM) != 1u)
        TEST_FAILF("ANIMATIONS did not advance to 1");
    /* the ANIMATIONS badge: codeb1 cells 2..3 at srcOff 0xC00, cell 1 = OFF */
    TEST_ASSERT_FALSE(_check_badge("ANIMATIONS", 0x0F88u, cb1, cb1Size, 0x0C00u,
                                   48u, 32u, 1u, 539u, 32u));
    _press_action(); // 1 -> 0: the wrap
    if (frontend_option(FRONTEND_OPT_ANIM) != 0u)
        TEST_FAILF("ANIMATIONS did not wrap at 2");
    _press_action(); // leave it OFF for the cutscene gate

    /* --- exit: walk off the right edge, carrying the settings out ----- */
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_LEFT, 4u, "left off the top rail"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_DOWN, 1u, "down to the walkway"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_DOWN, 0u, "down to the floor"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_RIGHT, 6u, "right to the exit rail"));
    TEST_ASSERT_FALSE(_walk_to(JOYBITS_RIGHT, 0u, "off the right edge"));
    {
        InputState in = _idle();
        uint32_t i;
        for (i = 0; i < 300u; i++) {
            if (!frontend_tick(&in)) {
                palette_tick();
                break;
            }
            palette_tick();
        }
        if (i == 300u) TEST_FAILF("the room did not exit");
    }

    /* attract_exit 1000:13E9 -> entry LAB_002B: after the attract (also the
     * one after the ending) the room's setup runs again.  frontend_restart
     * is that re-entry; without it the stale exit phase hands straight back
     * to the level.  The room must accept a _tick again. */
    frontend_restart();
    {
        InputState rin = _idle();

        if (!frontend_tick(&rin) || !frontend_tick(&rin))
            TEST_FAILF("the room did not re-enter after the attract exit "
                       "(frontend_restart)");
    }

    /* --- the name screen's input law (D-56/D-57) ---------------------- */
    {
        InputState in = _idle();
        uint32_t t;
        static const char wrong[] = "WRONG CODE";
        static const char code14[] = "FATEANDFORTUNE";

        /* hold left: the rail takes the character to x=0x108, then the pan
         * runs the scroll to 0 and the name screen opens */
        in.joyBits = JOYBITS_LEFT;
        for (t = 0; t < 800u && !frontend_name_screen_for_probe(); t++)
            _tick(&in);
        in = _idle();
        _tick(&in); // release the walk
        if (!frontend_name_screen_for_probe())
            TEST_FAILF("the name screen did not open");
        /* the typed name must land in the 1000:AB8D strip - destination
         * [0x5AC4]+0x38F5 + 0xA0/row, i.e. page rows ~0x5B..0x65.  An
         * 8-bit truncation of that address was invisible to the old
         * checks: it dropped the text at the top of the page. */
        {
            static uint8_t before[64000];
            const uint8_t *p0 = osys_null_page(0);
            uint32_t top = 200u, bottom = 0;

            memcpy(before, p0, sizeof before);
            in = _idle();
            in.lastKey = 'X';
            _tick(&in);
            _tick(&in); // the next compose presents the redrawn line
            p0 = osys_null_page(0);
            for (uint32_t y = 0; y < 200u; y++)
                for (uint32_t x = 0; x < 320u; x++)
                    if (p0[y * 320u + x] != before[y * 320u + x]) {
                        if (y < top) top = y;
                        if (y > bottom) bottom = y;
                    }
            if (top == 200u || top < 88u || bottom > 108u)
                TEST_FAILF("the typed name left its strip (rows %u..%u)", top,
                           bottom);
        }
        /* ESC clears the seeded DARWIN */
        in = _idle();
        in.lastKey = 0x1Bu;
        _tick(&in);
        if (strcmp(frontend_name_for_probe(), "") != 0)
            TEST_FAILF("ESC did not clear the name");
        /* 1000:AB1E's BX = 13 walk stops at the 14th character (the
         * BX < 0 guard); 20 keys must not write past the 15-byte
         * buffer.  The seeded-then-cleared state keeps the screen up. */
        for (uint32_t i = 0; i < 20u; i++) {
            in = _idle();
            in.lastKey = 'Z';
            _tick(&in);
        }
        if (strlen(frontend_name_for_probe()) > 14u)
            TEST_FAILF("the name buffer grew to %u chars",
                       (unsigned)strlen(frontend_name_for_probe()));
        in = _idle();
        in.lastKey = 0x1Bu;
        _tick(&in); // clear the Z run again
        /* type a wrong code and Enter: the miss resets DARWIN */
        for (uint32_t i = 0; wrong[i]; i++) {
            in = _idle();
            in.lastKey = (uint8_t)wrong[i];
            _tick(&in);
        }
        in = _idle();
        in.lastKey = 0x0Du;
        _tick(&in);
        if (strcmp(frontend_name_for_probe(), "DARWIN") != 0)
            TEST_FAILF("the miss reset is '%s'", frontend_name_for_probe());
        /* D-56: the tail is zeroed; one Backspace erases the visible N */
        in = _idle();
        in.lastKey = 0x0Eu;
        _tick(&in);
        if (strcmp(frontend_name_for_probe(), "DARWI") != 0)
            TEST_FAILF("the first Backspace left '%s'",
                       frontend_name_for_probe());
        /* D-56: typing after a miss must not expose a stale tail */
        in = _idle();
        in.lastKey = 0x0Du;
        _tick(&in); // DARWI -> miss
        in = _idle();
        in.lastKey = 'X';
        _tick(&in);
        if (strcmp(frontend_name_for_probe(), "DARWINX") != 0)
            TEST_FAILF("post-miss typing gave '%s'", frontend_name_for_probe());
        /* D-57: a 14-character code is accepted and matches level 155 */
        in = _idle();
        in.lastKey = 0x1Bu;
        _tick(&in);
        for (uint32_t i = 0; code14[i]; i++) {
            in = _idle();
            in.lastKey = (uint8_t)code14[i];
            _tick(&in);
        }
        if (strcmp(frontend_name_for_probe(), code14) != 0)
            TEST_FAILF("the 14-char code reads '%s'",
                       frontend_name_for_probe());
        in = _idle();
        in.lastKey = 0x0Du;
        _tick(&in);
        if (frontend_selected_level() != 155u)
            TEST_FAILF("FATEANDFORTUNE matched level %u",
                       frontend_selected_level());
    }

    if (frontend_option(FRONTEND_OPT_LEVEL) != 1u ||
        frontend_option(FRONTEND_OPT_GAME) != 1u ||
        frontend_option(FRONTEND_OPT_ANIM) != 1u)
        TEST_FAILF("the settings were not carried out (LEVEL %u, GAME %u, "
                   "ANIM %u)",
                   frontend_option(FRONTEND_OPT_LEVEL),
                   frontend_option(FRONTEND_OPT_GAME),
                   frontend_option(FRONTEND_OPT_ANIM));

    /* --- (c) LEVEL -> the level's own time B and the 10-member tribe --- */
    level_seed_tribe(); // entry 1000:009C..0x00B1, before the level walk
    if (level_load(0) != 0) TEST_FAILF("level 0 did not load");
    {
        const LevelData *d = level_data();
        if (level_time_limit() != d->timeB || d->timeB == d->timeA)
            TEST_FAILF("LEVEL did not pick time B (%04x, want %04x)",
                       (uint32_t)level_time_limit(), (uint32_t)d->timeB);
        if (level_tribe_members() != 10u)
            TEST_FAILF("LEVEL did not pick 10 members (got %u)",
                       level_tribe_members());
    }

    /* --- (c) ANIMATIONS -> cd_file_op's early RET --------------------- */
    if (scene_play_level_intro(0) != 0)
        TEST_FAILF("ANIMATIONS off did not stop the cutscene");

    free(cb1);
    free(cb2);
    free(rst);
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_frontend);
    return UNITY_END();
}
