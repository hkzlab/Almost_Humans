/* test_save.c - the RESTORE GAME slot's load (pass 8).
 *
 * Builds a byte-exact 0x1800 save fixture named
 * `SAVEB` - the fs module's file for slot 2 - then drives the front-end room
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
 * need them).  The fixture is written next to them - the module's save
 * location - and removed at the end.  A pre-existing SAVEB is never
 * clobbered: the test skips.
 * Missing volumes = skip. */
#include "audio.h"
#include "font.h"
#include "frontend.h"
#include "gameflow.h"
#include "level/level.h"
#include "null/osys_null.h"
#include "osystem.h"
#include "res.h"
#include "palette.h"
#include "save.h"
#include "score.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"
#include "byteorder.h"
#include "test_support.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SAVE_LEN  0x1800u
#define SAVE_NAME "SAVEB"

/**
 * @brief Store one little-endian 16-bit word.
 *
 * @param p the destination bytes
 * @param v the value
 */
static void _put16(uint8_t *p, uint32_t v) { wr_le_u16(p, (uint16_t)v); }

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

/**
 * @brief Run one flow step and its palette ISR step.
 *
 * @param s the state
 * @param in the input state
 * @return 1 when the flow state is FLOW_LEVEL, 0 otherwise
 */
static int32_t _tick(GameFlow *s, const InputState *in)
{
    *s = gameflow_step(*s, in);
    palette_tick();                 // game_tick's ISR body
    return *s == FLOW_LEVEL;
}

/** @brief Hold `dirBit` until the rail arrival changes the cell (or the room hands
 * over to LEVEL).  One _idle _tick afterwards so the rail cannot re-arm.
 *
 * @param s the state
 * @param dirBit the direction bit
 * @param want the wanted value
 * @param what the check description
 * @return 1 when the cell changed, 0 when the room exited
 */
static int32_t _walk_to(GameFlow *s, uint32_t dirBit, uint32_t want,
                        const char *what)
{
    uint32_t start = frontend_cell_for_probe();
    InputState in = _idle();
    in.joyBits = dirBit;
    for (uint32_t i = 0; i < 400u; i++) {
        if (_tick(s, &in)) return 0;             // the room exited
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

/**
 * @brief Press and release ACTION for one flow step.
 *
 * @param s the state
 * @return 1 when the next flow step leaves the room, 0 otherwise
 */
static int32_t _press_action(GameFlow *s)
{
    InputState in = _idle();
    in.joyBits = JOYBITS_FIRE;
    int32_t over = _tick(s, &in);
    InputState rel = _idle();
    return over | _tick(s, &rel);
}

/**
 * @brief Run the save/restore tests.
 */
static void test_save(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : ".";
    char path[512];

    /* the fixture: header + two recognisable blob bytes */
    static uint8_t fx[SAVE_LEN];
    memset(fx, 0, sizeof fx);
    _put16(fx + 0x00, 2);                 // the level -> LB#1
    _put16(fx + 0x02, 0x1234);            // score low
    _put16(fx + 0x04, 0x0001);            // score high
    fx[0x06] = 7;                        // the state id
    _put16(fx + 0x07, 0x0549);            // 5:49 remaining (BCD)
    fx[0x09] = 0;
    fx[0x0A] = 9;          // the counter digits
    _put16(fx + 0x0B, 7);                 // members: the board gives 12
    fx[0x0D] = 0xC3;                     // the blob's first byte
    fx[0x17F0] = 0x5A;                   // the parameter block

    snprintf(path, sizeof path, "%s/HUMANS.001", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        TEST_SKIPF("skip: no volume files at %s (fixture data absent)",
                   vol_dir);
    }
    fclose(f);
    if (compat_chdir(vol_dir)) { TEST_FAILF("cannot enter %s", vol_dir); }

    f = fopen(SAVE_NAME, "rb");
    if (f) {
        fclose(f);
        TEST_SKIPF("skip: %s exists in %s (a real save; not overwriting)",
                   SAVE_NAME, vol_dir);
    }
    f = fopen(SAVE_NAME, "wb");
    if (!f || fwrite(fx, 1, sizeof fx, f) != sizeof fx) {
        TEST_FAILF("cannot write the %s fixture", SAVE_NAME);
    }
    fclose(f);

    osys_bind(&g_osys_null);
    res_init();
    video_init();
    palette_init();
    font_init();
    audio_init();
    statusbar_init();
    level_init();
    frontend_init();
    save_init();

    /* the room: load + fade-in, then the walk to RESTORE GAME */
    GameFlow s = FLOW_MENU;
    {
        InputState in = _idle();
        for (uint32_t i = 0; i < 60u; i++) (void)_tick(&s, &in);
    }
    TEST_ASSERT_TRUE_MESSAGE(s == FLOW_MENU, "the room did not come up");
    TEST_ASSERT_FALSE(_walk_to(&s, 0x08u, 6u, "right to RESTORE GAME"));
    TEST_ASSERT_FALSE(_press_action(&s));              // 0 -> 1
    TEST_ASSERT_FALSE(_press_action(&s));              // 1 -> 2
    if (frontend_option(FRONTEND_OPT_GAME) != 2u) {
        TEST_FAILF("RESTORE GAME is %u, want 2",
                   frontend_option(FRONTEND_OPT_GAME));
    }
    TEST_ASSERT_FALSE(_walk_to(&s, 0x08u, 0u, "off the right edge"));
    {
        InputState in = _idle();
        uint32_t guard = 0;
        while (s == FLOW_MENU && guard++ < 300u) (void)_tick(&s, &in);
    }
    TEST_ASSERT_TRUE_MESSAGE(s == FLOW_LEVEL, "the room never reached LEVEL");

    /* entry:004C: the slot loaded and its level overrode the match */
    if (!save_present() || gameflow_level_current() != 1u) {
        TEST_FAILF("save not applied (present %d, level %u)", save_present(),
                   gameflow_level_current());
    }

    if (level_load(gameflow_level_current()) != 0) {
        TEST_FAILF("level %u did not load", gameflow_level_current());
    } else {
        const LevelData *d = level_data();
        if (d->levelId != 1u) {                 // LB#1 = level 2
            TEST_FAILF("the save's level did not win (%u)", d->levelId);
        }
        /* render_level_frame 1000:959E: the save's word, not the board pick */
        if (level_time_limit() != 0x0549u) {
            TEST_FAILF("the save's time was not picked (0x%04x)",
                       (uint32_t)level_time_limit());
        }
        if (level_tribe_members() != 7u) {
            TEST_FAILF("the save's members were not picked (%u)",
                       level_tribe_members());
        }
        /* the blob is carried verbatim */
        if (save_blob()[0x0D] != 0xC3u || save_blob()[0x17F0] != 0x5Au) {
            TEST_FAILF("the save blob was not carried");
        }
        /* entry:004C's state restore: the header score/counter */
        save_restore_state();
        if (score_total_lo() != 0x1234u || score_total_hi() != 1u ||
            statusbar_tribe_digit(0u) != 0u ||
            statusbar_tribe_digit(1u) != 9u) {
            TEST_FAILF("the save's score/counter did not restore");
        }
    }

    /* slot 0 = GAME OFF: the fresh path (LEVEL default EASY -> timeA/12) */
    if (save_load_slot(0u) != 0 || save_present()) {
        TEST_FAILF("slot 0 did not clear the save");
    }
    level_seed_tribe(); // entry 1000:009C..0x00B1
    if (level_load(0) != 0) {
        TEST_FAILF("the fresh level did not load");
    } else {
        const LevelData *d = level_data();
        if (level_time_limit() != d->timeA || level_tribe_members() != 12u) {
            TEST_FAILF("the fresh pick is not the board's "
                       "(time 0x%04x, members %u)",
                       (uint32_t)level_time_limit(), level_tribe_members());
        }
        /* D-265: only the entry seeds the count; a level transition keeps the
         * carried value and a restart restores that level's start count. */
        level_set_members(9u);
        if (level_load(1u) != 0) {
            TEST_FAILF("level 2 did not load");
        } else if (level_tribe_members() != 9u) {
            TEST_FAILF("the count did not carry (%u, want 9)",
                       level_tribe_members());
        }
        level_set_members(5u);
        level_restore_members();
        if (level_tribe_members() != 9u) {
            TEST_FAILF("the restart count is %u, want 9",
                       level_tribe_members());
        }
    }

    remove(SAVE_NAME);
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_save);
    return UNITY_END();
}
