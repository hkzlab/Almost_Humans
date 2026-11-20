/* test_boot_key.c - D-245: a held key aborts the boot intro.
 *
 * The original's [0x48A3] is a held-key level (INT 9 writes the code on a
 * make and 0 on the release).  play_scene_sequence tests it at 1000:b2ba,
 * and every bank/hold wait polls it through input_abort_check (1000:c0c6):
 * a key that is still down jumps 1000:b60a, fades to black and returns to
 * entry, whose 1000:0028 calls game_loop (the attract).
 *
 * The SDL backend used to expose only the per-make lastKey pulse, so the
 * first poll saw the key and the next saw an empty byte: the logo skipped
 * its remaining frames and the boot carried on through the cards and the
 * scene.  This test injects a held Return after the initial fade and
 * requires the abort phase, a black DAC and a finish in a few hundred
 * ticks instead of the full ~5,700.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * Missing volumes = skip. */

#include "audio.h"
#include "null/osys_null.h"
#include "osystem.h"
#include "res.h"
#include "palette.h"
#include "scene/scene.h"
#include "video.h"
#include "compat.h"
#include "test_support.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* BootPhase values (the private enum in scene.c) - the probe returns the
 * raw phase, and these are the two the test observes. */
enum {
    BOOT_ABORT_PHASE = 11
};

/**
 * @brief Run the held-key abort scenario.
 */
static void test_boot_key(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : ".";

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        TEST_SKIPF("skip: no volume files at %s (fixture data absent)",
                   vol_dir);
    fclose(f);
    if (compat_chdir(vol_dir)) TEST_FAILF("chdir %s", vol_dir);

    osys_bind(&g_osys_null);
    res_init();
    scene_init();
    video_init();
    palette_init();

    /* the idle prefix: the boot's fade to black and the first logo groups */
    InputState idle;
    memset(&idle, 0, sizeof idle);
    scene_note_input(&idle);

    uint8_t r = 1;
    uint32_t t = 0;
    for (; t < 50u && r == 1; t++) {
        r = scene_play_boot_intro();
        palette_tick();
        audio_tick();
    }
    if (r != 1) TEST_FAILF("the boot finished during the 50-tick idle prefix");

    /* A physically held Return whose make pulse was already consumed on an
     * earlier poll: only the level is left, exactly the case the one-tick
     * lastKey model lost. */
    InputState held;
    memset(&held, 0, sizeof held);
    held.keyHeld = 1u;
    scene_note_input(&held);

    int32_t saw_abort = 0;
    uint32_t held_ticks = 0;
    while (r == 1 && held_ticks < 300u) {
        if (scene_boot_phase_for_probe() == BOOT_ABORT_PHASE) saw_abort = 1;
        r = scene_play_boot_intro();
        palette_tick();
        audio_tick();
        held_ticks++;
    }

    if (r != 0)
        TEST_FAILF("the held key did not abort the boot (%u ticks, phase %u)",
                   held_ticks, scene_boot_phase_for_probe());
    if (!saw_abort) TEST_FAILF("the boot ended without taking BOOT_ABORT");
    if (held_ticks > 300u) TEST_FAILF("the abort took %u ticks", held_ticks);

    /* b60a: clear_fade_ramp + the 0x100-entry fade div 0x23 to black */
    {
        const uint8_t *pal = osys_null_palette();
        for (uint32_t i = 0; i < 768u; i++) {
            if (pal[i] != 0u)
                TEST_FAILF("the abort fade left palette byte %u = %u", i,
                           pal[i]);
        }
    }
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_boot_key);
    return UNITY_END();
}
