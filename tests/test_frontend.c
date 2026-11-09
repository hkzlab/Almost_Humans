/* test_frontend.c - the front-end room's option read side (the ACTION key).
 *
 * Drives frontend_tick with synthetic input: walks the rails to each option
 * cell, presses ACTION (joyBits bit 7), and asserts:
 *   (a) the counter advances and wraps at the cell's count;
 *   (b) the composed page's badge pixels are the next shipped sprite cell
 *       (byte-checked against CODEB1/CODEB2/RESTORE, the files the placer
 *       reads: a cell's opaque pixels must equal the page's);
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
#include "level.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "scene.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);
extern uint32_t osys_null_audio_starts(void);
extern uint32_t osys_null_audio_stops(void);
extern VideoMode osys_null_video_mode(void);

extern const OSystem g_osys_null;

/**
 * @brief Run one frontend tick and its palette ISR step.
 *
 * @param in the input state
 */
static void _tick(const InputState *in)
{
    frontend_tick(in);
    palette_tick();                 /* game_tick's ISR body */
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
    in.joyBits = 0x80u;
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
            if (!v) continue;              /* nonzero-transparent */
            if (wx >= charX + 2u && wx <= charX + 17u && wy >= charY &&
                wy <= charY + 23u)
                continue;   /* the walker */
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
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    char path[512];

    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
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

    int32_t bad = 0;

    res_init(&g_osys_null);
    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    font_init(&g_osys_null);
    audio_init(&g_osys_null);
    statusbar_init(&g_osys_null);
    level_init(&g_osys_null);
    scene_init(&g_osys_null);
    frontend_init(&g_osys_null);

    /* the shipped badge art (the same chunks the placer reads, from disc 0) */
    uint32_t cb1Size = 0, cb2Size = 0, rstSize = 0;
    uint8_t *cb1 = res_load_chunk("APL\\CODEB1.SPR", &cb1Size, NULL);
    uint8_t *cb2 = res_load_chunk("APL\\CODEB2.SPR", &cb2Size, NULL);
    uint8_t *rst = res_load_chunk("APL\\RESTORE.SPR", &rstSize, NULL);
    if (!cb1 || !cb2 || !rst || cb1Size != 4u * 48u * 32u ||
        cb2Size != 3u * 80u * 32u || rstSize != 21u * 48u * 32u) {
        fprintf(stderr, "FAIL badge assets unreadable\n");
        return 1;
    }

    /* (c) ANIMATIONS is ON by default: the cutscene starts.  (Done before
     * the room so the value is still 0; the scene then stays mid-play and
     * the post-room call below is a pure gate check.) */
    if (frontend_option(FRONTEND_OPT_ANIM) != 0u ||
        scene_play_level_intro(0) != 1) {
        fprintf(stderr, "FAIL the level intro did not start with "
                        "ANIMATIONS on\n");
        bad = 1;
    }
    res_set_disc(0u);           /* the room's assets ship on disc 0 */

    /* enter the room: load + the fade-in (~24 ticks), then _idle */
    {
        InputState in = _idle();
        for (uint32_t i = 0; i < 60u; i++) _tick(&in);
    }
    if (osys_null_audio_starts() != 1u || osys_null_audio_stops() != 0u) {
        fprintf(stderr,
                "FAIL MUSIC on should resume once (starts %u, "
                "stops %u)\n",
                osys_null_audio_starts(), osys_null_audio_stops());
        bad = 1;
    }
    if (osys_null_video_mode() != VIDEO_MODE_X) {
        fprintf(stderr, "FAIL the room did not set the mode-X field\n");
        bad = 1;
    }
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
        if (!lit) {
            fprintf(stderr,
                    "FAIL the backdrop's rows past the image are blanked\n");
            bad = 1;
        }
    }

    /* --- LEVEL: cell 2 (EASY/NORMAL/HARD = 3 states) ------------------- */
    bad |= _walk_to(0x01u, 1u, "up to the walkway");
    bad |= _walk_to(0x04u, 2u, "left to LEVEL");
    if (frontend_option(FRONTEND_OPT_LEVEL) != 0u) {
        fprintf(stderr, "FAIL LEVEL starts at %u, want 0\n",
                frontend_option(FRONTEND_OPT_LEVEL));
        bad = 1;
    }
    _press_action();
    if (frontend_option(FRONTEND_OPT_LEVEL) != 1u) {
        fprintf(stderr, "FAIL LEVEL did not advance to 1\n");
        bad = 1;
    }
    /* the LEVEL badge: codeb2, dest 0x1C80, 80x32, cell 1 (NORMAL) */
    bad |= _check_badge("LEVEL", 0x1C80u, cb2, cb2Size, 0u, 80u, 32u, 1u, 400u,
                        55u);
    _press_action();
    _press_action();                     /* 2 then the wrap to 0 */
    if (frontend_option(FRONTEND_OPT_LEVEL) != 0u) {
        fprintf(stderr, "FAIL LEVEL did not wrap at 3\n");
        bad = 1;
    }
    _press_action(); /* leave it on NORMAL for the level */

    /* --- MUSIC: cell 3 (ON/OFF = 2 states) ----------------------------- */
    bad |= _walk_to(0x04u, 3u, "left to MUSIC");
    _press_action(); /* 0 -> 1: OFF */
    if (frontend_option(FRONTEND_OPT_MUSIC) != 1u) {
        fprintf(stderr, "FAIL MUSIC did not advance to 1\n");
        bad = 1;
    }
    if (osys_null_audio_stops() != 1u || osys_null_audio_starts() != 1u) {
        fprintf(stderr,
                "FAIL MUSIC off did not stop the backend (starts "
                "%u, stops %u)\n",
                osys_null_audio_starts(), osys_null_audio_stops());
        bad = 1;
    }
    /* the MUSIC badge: codeb1 cell 1 (OFF), dest 0x1D14, 48x32 */
    bad |= _check_badge("MUSIC", 0x1D14u, cb1, cb1Size, 0u, 48u, 32u, 1u, 362u,
                        55u);
    _press_action(); /* 1 -> 0: the wrap; resume */
    if (frontend_option(FRONTEND_OPT_MUSIC) != 0u ||
        osys_null_audio_starts() != 2u) {
        fprintf(stderr,
                "FAIL MUSIC did not wrap/resume (counter %u, "
                "starts %u)\n",
                frontend_option(FRONTEND_OPT_MUSIC), osys_null_audio_starts());
        bad = 1;
    }
    {
        InputState in = _idle();
        for (uint32_t i = 0; i < 5u; i++) _tick(&in); /* the latch holds */
    }
    if (osys_null_audio_starts() != 2u || osys_null_audio_stops() != 1u) {
        fprintf(stderr, "FAIL the music latch re-called the backend\n");
        bad = 1;
    }

    /* --- on to RESTORE GAME: cell 6 (OFF/1..20 = 21 states) ----------- */
    bad |= _walk_to(0x08u, 2u, "right back to LEVEL");
    bad |= _walk_to(0x08u, 1u, "right to the floor rail");
    bad |= _walk_to(0x02u, 0u, "down to the floor");
    bad |= _walk_to(0x08u, 6u, "right to RESTORE GAME");
    _press_action(); /* 0 -> 1 */
    if (frontend_option(FRONTEND_OPT_GAME) != 1u) {
        fprintf(stderr, "FAIL RESTORE GAME did not advance to 1\n");
        bad = 1;
    }
    /* the RESTORE GAME badge: restore.spr cell 1 (GAME 1), dest 0x5BBC */
    bad |= _check_badge("RESTORE GAME", 0x5BBCu, rst, rstSize, 0u, 48u, 32u, 1u,
                        520u, 160u);
    for (uint32_t i = 0; i < 20u; i++) _press_action(); /* 2..20, then wrap */
    if (frontend_option(FRONTEND_OPT_GAME) != 0u) {
        fprintf(stderr, "FAIL RESTORE GAME did not wrap at 21\n");
        bad = 1;
    }
    _press_action(); /* leave GAME 1 for the exit */

    /* --- ANIMATIONS: cell 5 (ON/OFF = 2 states) ----------------------- */
    bad |= _walk_to(0x04u, 0u, "left to the floor start");
    bad |= _walk_to(0x01u, 1u, "up to the walkway");
    bad |= _walk_to(0x01u, 4u, "up to the top rail");
    bad |= _walk_to(0x08u, 5u, "right to ANIMATIONS");
    _press_action(); /* 0 -> 1: OFF */
    if (frontend_option(FRONTEND_OPT_ANIM) != 1u) {
        fprintf(stderr, "FAIL ANIMATIONS did not advance to 1\n");
        bad = 1;
    }
    /* the ANIMATIONS badge: codeb1 cells 2..3 at srcOff 0xC00, cell 1 = OFF */
    bad |= _check_badge("ANIMATIONS", 0x0F88u, cb1, cb1Size, 0x0C00u, 48u, 32u,
                        1u, 539u, 32u);
    _press_action(); /* 1 -> 0: the wrap */
    if (frontend_option(FRONTEND_OPT_ANIM) != 0u) {
        fprintf(stderr, "FAIL ANIMATIONS did not wrap at 2\n");
        bad = 1;
    }
    _press_action(); /* leave it OFF for the cutscene gate */

    /* --- exit: walk off the right edge, carrying the settings out ----- */
    bad |= _walk_to(0x04u, 4u, "left off the top rail");
    bad |= _walk_to(0x02u, 1u, "down to the walkway");
    bad |= _walk_to(0x02u, 0u, "down to the floor");
    bad |= _walk_to(0x08u, 6u, "right to the exit rail");
    bad |= _walk_to(0x08u, 0u, "off the right edge");
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
        if (i == 300u) {
            fprintf(stderr, "FAIL the room did not exit\n");
            bad = 1;
        }
    }

    /* attract_exit 1000:13E9 -> entry LAB_002B: after the attract (also the
     * one after the ending) the room's setup runs again.  frontend_restart
     * is that re-entry; without it the stale exit phase hands straight back
     * to the level.  The room must accept a _tick again. */
    frontend_restart();
    {
        InputState rin = _idle();

        if (!frontend_tick(&rin) || !frontend_tick(&rin)) {
            fprintf(stderr, "FAIL the room did not re-enter after the attract "
                            "exit (frontend_restart)\n");
            bad = 1;
        }
    }

    /* --- the name screen's input law (D-56/D-57) ---------------------- */
    {
        InputState in = _idle();
        uint32_t t;
        static const char wrong[] = "WRONG CODE";
        static const char code14[] = "FATEANDFORTUNE";

        /* hold left: the rail takes the character to x=0x108, then the pan
         * runs the scroll to 0 and the name screen opens */
        in.joyBits = 0x04u;
        for (t = 0; t < 800u && !frontend_name_screen_for_probe(); t++)
            _tick(&in);
        in = _idle();
        _tick(&in); /* release the walk */
        if (!frontend_name_screen_for_probe()) {
            fprintf(stderr, "FAIL the name screen did not open\n");
            bad = 1;
        } else {
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
                _tick(&in); /* the next compose presents the redrawn line */
                p0 = osys_null_page(0);
                for (uint32_t y = 0; y < 200u; y++)
                    for (uint32_t x = 0; x < 320u; x++)
                        if (p0[y * 320u + x] != before[y * 320u + x]) {
                            if (y < top) top = y;
                            if (y > bottom) bottom = y;
                        }
                if (top == 200u || top < 88u || bottom > 108u) {
                    fprintf(stderr,
                            "FAIL the typed name left its strip (rows %u..%u)\n",
                            (unsigned)top, (unsigned)bottom);
                    bad = 1;
                }
            }
            /* ESC clears the seeded DARWIN */
            in = _idle();
            in.lastKey = 0x1Bu;
            _tick(&in);
            if (strcmp(frontend_name_for_probe(), "") != 0) {
                fprintf(stderr, "FAIL ESC did not clear the name\n");
                bad = 1;
            }
            /* 1000:AB1E's BX = 13 walk stops at the 14th character (the
             * BX < 0 guard); 20 keys must not write past the 15-byte
             * buffer.  The seeded-then-cleared state keeps the screen up. */
            for (uint32_t i = 0; i < 20u; i++) {
                in = _idle();
                in.lastKey = 'Z';
                _tick(&in);
            }
            if (strlen(frontend_name_for_probe()) > 14u) {
                fprintf(stderr, "FAIL the name buffer grew to %u chars\n",
                        (unsigned)strlen(frontend_name_for_probe()));
                bad = 1;
            }
            in = _idle();
            in.lastKey = 0x1Bu;
            _tick(&in); /* clear the Z run again */
            /* type a wrong code and Enter: the miss resets DARWIN */
            for (uint32_t i = 0; wrong[i]; i++) {
                in = _idle();
                in.lastKey = (uint8_t)wrong[i];
                _tick(&in);
            }
            in = _idle();
            in.lastKey = 0x0Du;
            _tick(&in);
            if (strcmp(frontend_name_for_probe(), "DARWIN") != 0) {
                fprintf(stderr, "FAIL the miss reset is '%s'\n",
                        frontend_name_for_probe());
                bad = 1;
            }
            /* D-56: the tail is zeroed; one Backspace erases the visible N */
            in = _idle();
            in.lastKey = 0x0Eu;
            _tick(&in);
            if (strcmp(frontend_name_for_probe(), "DARWI") != 0) {
                fprintf(stderr, "FAIL the first Backspace left '%s'\n",
                        frontend_name_for_probe());
                bad = 1;
            }
            /* D-56: typing after a miss must not expose a stale tail */
            in = _idle();
            in.lastKey = 0x0Du;
            _tick(&in); /* DARWI -> miss */
            in = _idle();
            in.lastKey = 'X';
            _tick(&in);
            if (strcmp(frontend_name_for_probe(), "DARWINX") != 0) {
                fprintf(stderr, "FAIL post-miss typing gave '%s'\n",
                        frontend_name_for_probe());
                bad = 1;
            }
            /* D-57: a 14-character code is accepted and matches level 155 */
            in = _idle();
            in.lastKey = 0x1Bu;
            _tick(&in);
            for (uint32_t i = 0; code14[i]; i++) {
                in = _idle();
                in.lastKey = (uint8_t)code14[i];
                _tick(&in);
            }
            if (strcmp(frontend_name_for_probe(), code14) != 0) {
                fprintf(stderr, "FAIL the 14-char code reads '%s'\n",
                        frontend_name_for_probe());
                bad = 1;
            }
            in = _idle();
            in.lastKey = 0x0Du;
            _tick(&in);
            if (frontend_selected_level() != 155u) {
                fprintf(stderr, "FAIL FATEANDFORTUNE matched level %u\n",
                        frontend_selected_level());
                bad = 1;
            }
        }
    }

    if (frontend_option(FRONTEND_OPT_LEVEL) != 1u ||
        frontend_option(FRONTEND_OPT_GAME) != 1u ||
        frontend_option(FRONTEND_OPT_ANIM) != 1u) {
        fprintf(stderr,
                "FAIL the settings were not carried out (LEVEL %u, "
                "GAME %u, ANIM %u)\n",
                frontend_option(FRONTEND_OPT_LEVEL),
                frontend_option(FRONTEND_OPT_GAME),
                frontend_option(FRONTEND_OPT_ANIM));
        bad = 1;
    }

    /* --- (c) LEVEL -> the level's own time B and the 10-member tribe --- */
    if (level_load(0) != 0) {
        fprintf(stderr, "FAIL level 0 did not load\n");
        return 1;
    }
    {
        const LevelData *d = level_data();
        if (level_time_limit() != d->timeB || d->timeB == d->timeA) {
            fprintf(stderr,
                    "FAIL LEVEL did not pick time B (%04x, want "
                    "%04x)\n",
                    (uint32_t)level_time_limit(), (uint32_t)d->timeB);
            bad = 1;
        }
        if (level_tribe_members() != 10u) {
            fprintf(stderr, "FAIL LEVEL did not pick 10 members (got %u)\n",
                    level_tribe_members());
            bad = 1;
        }
    }

    /* --- (c) ANIMATIONS -> cd_file_op's early RET --------------------- */
    if (scene_play_level_intro(0) != 0) {
        fprintf(stderr, "FAIL ANIMATIONS off did not stop the cutscene\n");
        bad = 1;
    }

    free(cb1);
    free(cb2);
    free(rst);
    if (bad) {
        fprintf(stderr, "frontend: FAIL\n");
        return 1;
    }
    printf("frontend: ok\n");
    return 0;
}
