/* test_party_reset.c — D-49: level_intro_screen (1000:2508) is a
 * self-contained blocking routine, entered fresh on every level.  The port's
 * split start/tick must therefore re-initialise the briefing machine's
 * statics; before the fix a second briefing inherited the first one's final
 * PH_DONE and its first tick returned 0 (the screen was skipped).
 *
 * argv[1] = the volume dir (holding HUMANS.000/001/002).  Missing data =
 * skip with a notice, like the other fixture tests. */
#include "game.h"
#include "compat.h"
#include <stdio.h>
#include <string.h>

extern const OSystem osys_null;

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";

    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }
    if (res_init(&osys_null) || palette_init(&osys_null) ||
        font_init(&osys_null) || video_init(&osys_null)) {
        printf("PARTY_RESET: init failed\n");
        return 1;
    }
    res_setDisc(1u);                      /* the party art's volume */
    party_intro_setValues(1u, 4u, 12u, 0u, "DARWIN");
    if (!party_intro_start()) {
        printf("PARTY_RESET: skip (no party data)\n");
        return 0;
    }

    /* play the first briefing and dismiss it with a key */
    {
        InputState in;
        int32_t run = 1;
        uint32_t t = 0;

        memset(&in, 0, sizeof in);
        for (; t < 400u; t++) {
            (void)party_intro_tick(&in);
            palette_tick();
        }
        in.lastKey = 1u;
        for (; t < 20000u; t++) {
            run = party_intro_tick(&in);
            palette_tick();
            in.lastKey = 0u;
            if (!run) break;
        }
        if (run) {
            printf("PARTY_RESET: FAIL the first briefing never finished\n");
            return 1;
        }
    }

    /* the next level's briefing: its first tick must still run */
    party_intro_setValues(2u, 4u, 12u, 0u, "DARWIN");
    if (!party_intro_start()) {
        printf("PARTY_RESET: FAIL the second briefing did not start\n");
        return 1;
    }
    {
        InputState k;

        memset(&k, 0, sizeof k);
        if (!party_intro_tick(&k)) {
            printf("PARTY_RESET: FAIL the second briefing was skipped "
                   "(stale machine state)\n");
            return 1;
        }
    }
    printf("PARTY_RESET: OK\n");
    return 0;
}
