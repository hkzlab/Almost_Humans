/* test_boot_scene_key.c - D-247: a held key aborts the INTRO.RSC scene.
 *
 * load_intro_scene's loop polls the held-key abort twice: c0c6 after every
 * frame's wait (2 ticks at 1000:b787, 4 in the port - D-248) and c0af in the
 * transition's white-fade
 * wait (1000:b863).  Both jump 1000:b60a on a held key/fire.  The port used
 * to run the scene to the final 0x8C-tick hold instead.
 *
 * argv[1] = the game data directory; argv[2] = "trans" (inject during the
 * first transition's fade) or "frame" (inject on the first frame after that
 * transition).  The two modes pin the two poll sites; run once each.
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

/* BootPhase values (the private enum in scene.c). */
enum {
    BOOT_ABORT_PHASE = 11,
    BOOT_FRAME_PHASE = 25,
    BOOT_TRANS_PHASE = 28
};

/**
 * @brief Run one held-key scenario.
 */
static void test_boot_scene_key(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : ".";
    int trans = _test_argc > 2 && strcmp(_test_argv[2], "trans") == 0;

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

    InputState idle;
    memset(&idle, 0, sizeof idle);
    scene_note_input(&idle);

    uint8_t r = 1;
    uint32_t t = 0;
    int32_t seen_trans = 0, injected = 0;

    while (r == 1 && t < 8000u) {
        uint32_t ph = scene_boot_phase_for_probe();

        if (trans) {
            /* the white-fade wait: phase 28 with the fade still stepping */
            if (ph == BOOT_TRANS_PHASE && palette_fade_active()) {
                injected = 1;
                break;
            }
        } else {
            /* the first frame after the frame-0 transition has finished */
            if (ph == BOOT_TRANS_PHASE) seen_trans = 1;
            if (seen_trans && ph == BOOT_FRAME_PHASE) {
                injected = 1;
                break;
            }
        }
        r = scene_play_boot_intro();
        palette_tick();
        audio_tick();
        t++;
    }

    if (!injected)
        TEST_FAILF("the boot never reached the %s injection point "
                   "(tick %u, phase %u, r %d)",
                   trans ? "transition" : "post-transition frame", t,
                   scene_boot_phase_for_probe(), r);

    /* a physically held key (the make pulse may already be consumed) */
    InputState held;
    memset(&held, 0, sizeof held);
    held.keyHeld = 1u;
    scene_note_input(&held);

    int32_t saw_abort = 0;
    uint32_t abort_at = 0;
    uint32_t held_ticks = 0;
    while (r == 1 && held_ticks < 300u) {
        if (scene_boot_phase_for_probe() == BOOT_ABORT_PHASE) {
            if (!saw_abort) abort_at = held_ticks;
            saw_abort = 1;
        }
        r = scene_play_boot_intro();
        palette_tick();
        audio_tick();
        held_ticks++;
    }

    if (r != 0)
        TEST_FAILF("the held key did not abort the scene (%u ticks, phase %u)",
                   held_ticks, scene_boot_phase_for_probe());
    if (!saw_abort) TEST_FAILF("the scene ended without taking BOOT_ABORT");
    if (abort_at > (trans ? 2u : 5u))
        TEST_FAILF("the abort came at tick %u, not from the %s poll", abort_at,
                   trans ? "c0af fade" : "b787 frame-wait");
    if (held_ticks > 300u)
        TEST_FAILF("the scene abort took %u ticks", held_ticks);

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
    RUN_TEST(test_boot_scene_key);
    return UNITY_END();
}
