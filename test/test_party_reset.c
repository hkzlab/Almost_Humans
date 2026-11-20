/* test_party_reset.c - D-49: level_intro_screen (1000:2508) is a
 * self-contained blocking routine, entered fresh on every level.  The port's
 * split start/tick must therefore re-initialise the briefing machine's
 * statics; before the fix a second briefing inherited the first one's final
 * PH_DONE and its first tick returned 0 (the screen was skipped).
 *
 * argv[1] = the volume dir (holding HUMANS.000/001/002).  Missing data =
 * skip with a notice, like the other fixture tests. */
#include "audio.h"
#include "font.h"
#include "null/osys_null.h"
#include "osystem.h"
#include "palette.h"
#include "party_intro.h"
#include "res.h"
#include "video.h"
#include "compat.h"
#include "test_support.h"

#include <stdint.h>
#include <string.h>

/**
 * @brief Run the party-reset regression test.
 */
static void test_party_reset(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : ".";

    if (compat_chdir(vol_dir)) { TEST_FAILF("cannot enter %s", vol_dir); }
    osys_bind(&g_osys_null);
    if (res_init() || palette_init() || font_init() || video_init()) {
        TEST_FAILF("PARTY_RESET: init failed");
    }
    res_set_disc(1u);                      // the party art's volume
    party_intro_set_values(1u, 4u, 12u, 0u, "DARWIN");
    if (!party_intro_start()) {
        TEST_SKIPF("PARTY_RESET: skip (no party data)");
    }

    /* play the first briefing and dismiss it with a key */
    {
        InputState in;
        uint8_t run = 1;
        uint32_t t = 0;

        memset(&in, 0, sizeof in);
        for (; t < 400u; t++) {
            (void)party_intro_tick(&in);
            palette_tick();
            audio_tick();
        }
        in.lastKey = 1u;
        in.joyBits = JOYBITS_FIRE;
        for (; t < 20000u; t++) {
            run = party_intro_tick(&in);
            palette_tick();
            audio_tick();
            in.lastKey = 0u;
            if (!run) break;
        }
        TEST_ASSERT_FALSE_MESSAGE(run, "the first briefing never finished");
    }

    /* the next level's briefing: its first tick must still run */
    party_intro_set_values(2u, 4u, 12u, 0u, "DARWIN");
    TEST_ASSERT_TRUE_MESSAGE(party_intro_start(), "the second briefing did not "
                                                  "start");
    {
        InputState k;

        memset(&k, 0, sizeof k);
        TEST_ASSERT_TRUE_MESSAGE(party_intro_tick(&k),
                                 "the second briefing was skipped (stale "
                                 "machine state)");
    }
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_party_reset);
    return UNITY_END();
}
