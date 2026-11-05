/* test_save.c — the RESTORE GAME slot's load (pass 8).
 *
 * Builds a byte-exact 0x1800 save fixture (re_formats.md §10/§10.0) named
 * `SAVEB` — the fs module's file for slot 2 — then drives the front-end room
 * with synthetic input: walk to RESTORE GAME, press ACTION twice (GAME 2),
 * walk off the right edge.  Asserts:
 *   - the slot reached the loader (save_present, [0x34]);
 *   - the save's +0x00 overrides the room's code match (level 2 = LB#1, not
 *     the room's DARWIN/level 1);
 *   - the save's +0x07 time word and +0x0B members replace the LEVEL-board
 *     pick (level.c's save branch, render_level_frame 1000:959E);
 *   - the state blob is carried verbatim (the ranges have no consumer yet).
 * Then slot 0 asserts the fresh path is untouched.
 *
 * argv[1] = the game data directory (the volumes; the room and the level
 * need them).  The fixture is written next to them — the module's save
 * location — and removed at the end.  A pre-existing SAVEB is never
 * clobbered: the test skips.
 * Missing volumes = skip. */
#include "audio.h"
#include "font.h"
#include "frontend.h"
#include "gameflow.h"
#include "level.h"
#include "osystem.h"
#include "palette.h"
#include "save.h"
#include "score.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_LEN  0x1800u
#define SAVE_NAME "SAVEB"

extern const OSystem g_osys_null;

/** @brief Store one little-endian 16-bit word. */
static void _put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/** @brief A zeroed input state (no key, fire released). */
static InputState _idle(void)
{
    InputState in;
    memset(&in, 0, sizeof in);
    return in;
}

/** @brief Run one flow step and its palette ISR step. */
static int32_t _tick(GameFlow *s, const InputState *in)
{
    *s = gameflow_step(*s, in);
    palette_tick();                 /* game_tick's ISR body */
    return *s == FLOW_LEVEL;
}

/** Hold `dirBit` until the rail arrival changes the cell (or the room hands
 * over to LEVEL).  One _idle _tick afterwards so the rail cannot re-arm. */
static int32_t _walk_to(GameFlow *s, uint32_t dirBit, uint32_t want,
                        const char *what)
{
    uint32_t start = frontend_cell_for_probe();
    InputState in = _idle();
    in.joyBits = dirBit;
    for (uint32_t i = 0; i < 400u; i++) {
        if (_tick(s, &in)) return 0;             /* the room exited */
        if (frontend_cell_for_probe() != start) break;
    }
    if (frontend_cell_for_probe() != want) {
        fprintf(stderr, "FAIL walk %s: cell %u, want %u\n", what,
                frontend_cell_for_probe(), want);
        return 1;
    }
    InputState rel = _idle();
    (void)_tick(s, &rel);
    return 0;
}

/** @brief Press and release ACTION for one flow step. */
static int32_t _press_action(GameFlow *s)
{
    InputState in = _idle();
    in.joyBits = 0x80u;
    int32_t over = _tick(s, &in);
    InputState rel = _idle();
    return over | _tick(s, &rel);
}

/** @brief Run the save/restore tests. */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    char path[512];
    int32_t bad = 0;

    /* the fixture: header + two recognisable blob bytes */
    static uint8_t fx[SAVE_LEN];
    memset(fx, 0, sizeof fx);
    _put16(fx + 0x00, 2);                 /* the level -> LB#1          */
    _put16(fx + 0x02, 0x1234);            /* score low                  */
    _put16(fx + 0x04, 0x0001);            /* score high                 */
    fx[0x06] = 7;                        /* the state id               */
    _put16(fx + 0x07, 0x0549);            /* 5:49 remaining (BCD)       */
    fx[0x09] = 0;
    fx[0x0A] = 9;          /* the counter digits         */
    _put16(fx + 0x0B, 7);                 /* members: the board gives 12 */
    fx[0x0D] = 0xC3;                     /* the blob's first byte      */
    fx[0x17F0] = 0x5A;                   /* the parameter block        */

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

    f = fopen(SAVE_NAME, "rb");
    if (f) {
        fclose(f);
        printf("skip: %s exists in %s (a real save; not overwriting)\n",
               SAVE_NAME, vol_dir);
        return 0;
    }
    f = fopen(SAVE_NAME, "wb");
    if (!f || fwrite(fx, 1, sizeof fx, f) != sizeof fx) {
        if (f) fclose(f);
        fprintf(stderr, "FAIL cannot write the %s fixture\n", SAVE_NAME);
        return 1;
    }
    fclose(f);

    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    font_init(&g_osys_null);
    audio_init(&g_osys_null);
    statusbar_init(&g_osys_null);
    level_init(&g_osys_null);
    frontend_init(&g_osys_null);
    save_init(&g_osys_null);

    /* the room: load + fade-in, then the walk to RESTORE GAME */
    GameFlow s = FLOW_MENU;
    {
        InputState in = _idle();
        for (uint32_t i = 0; i < 60u; i++) (void)_tick(&s, &in);
    }
    if (s != FLOW_MENU) {
        fprintf(stderr, "FAIL the room did not come up\n");
        bad = 1;
    }
    bad |= _walk_to(&s, 0x08u, 6u, "right to RESTORE GAME");
    bad |= _press_action(&s);              /* 0 -> 1 */
    bad |= _press_action(&s);              /* 1 -> 2 */
    if (frontend_option(FRONTEND_OPT_GAME) != 2u) {
        fprintf(stderr, "FAIL RESTORE GAME is %u, want 2\n",
                frontend_option(FRONTEND_OPT_GAME));
        bad = 1;
    }
    bad |= _walk_to(&s, 0x08u, 0u, "off the right edge");
    {
        InputState in = _idle();
        uint32_t guard = 0;
        while (s == FLOW_MENU && guard++ < 300u) (void)_tick(&s, &in);
    }
    if (s != FLOW_LEVEL) {
        fprintf(stderr, "FAIL the room never reached LEVEL\n");
        bad = 1;
    }

    /* entry:004C: the slot loaded and its level overrode the match */
    if (!save_present() || gameflow_level_for_probe() != 1u) {
        fprintf(stderr, "FAIL save not applied (present %d, level %u)\n",
                save_present(), gameflow_level_for_probe());
        bad = 1;
    }

    if (level_load(gameflow_level_for_probe()) != 0) {
        fprintf(stderr, "FAIL level %u did not load\n",
                gameflow_level_for_probe());
        bad = 1;
    } else {
        const LevelData *d = level_data();
        if (d->levelId != 1u) {                 /* LB#1 = level 2 */
            fprintf(stderr, "FAIL the save's level did not win (%u)\n",
                    d->levelId);
            bad = 1;
        }
        /* render_level_frame 1000:959E: the save's word, not the board pick */
        if (level_time_limit() != 0x0549u) {
            fprintf(stderr, "FAIL the save's time was not picked (0x%04x)\n",
                    (uint32_t)level_time_limit());
            bad = 1;
        }
        if (level_tribe_members() != 7u) {
            fprintf(stderr, "FAIL the save's members were not picked (%u)\n",
                    level_tribe_members());
            bad = 1;
        }
        /* the blob is carried verbatim (offsets per re_formats.md §10) */
        if (save_blob()[0x0D] != 0xC3u || save_blob()[0x17F0] != 0x5Au) {
            fprintf(stderr, "FAIL the save blob was not carried\n");
            bad = 1;
        }
        /* entry:004C's state restore: the header score/counter */
        save_restore_state();
        if (score_total_lo() != 0x1234u || score_total_hi() != 1u ||
            statusbar_tribe_digit(0u) != 0u ||
            statusbar_tribe_digit(1u) != 9u) {
            fprintf(stderr, "FAIL the save's score/counter did not restore\n");
            bad = 1;
        }
    }

    /* slot 0 = GAME OFF: the fresh path (LEVEL default EASY -> timeA/12) */
    if (save_load_slot(0u) != 0 || save_present()) {
        fprintf(stderr, "FAIL slot 0 did not clear the save\n");
        bad = 1;
    }
    if (level_load(0) != 0) {
        fprintf(stderr, "FAIL the fresh level did not load\n");
        bad = 1;
    } else {
        const LevelData *d = level_data();
        if (level_time_limit() != d->timeA || level_tribe_members() != 12u) {
            fprintf(stderr,
                    "FAIL the fresh pick is not the board's "
                    "(time 0x%04x, members %u)\n",
                    (uint32_t)level_time_limit(), level_tribe_members());
            bad = 1;
        }
    }

    remove(SAVE_NAME);
    if (bad) {
        fprintf(stderr, "save: FAIL\n");
        return 1;
    }
    printf("save: ok\n");
    return 0;
}
