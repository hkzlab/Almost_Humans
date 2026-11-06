/* test_boot_key.c — D-245: a held key aborts the boot intro.
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

#include "osystem.h"
#include "palette.h"
#include "scene.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <string.h>

extern const uint8_t *osys_null_palette(void);
extern const OSystem g_osys_null;

/* BootPhase values (the private enum in scene.c) — the probe returns the
 * raw phase, and these are the two the test observes. */
enum {
    BOOT_ABORT_PHASE = 11
};

/**
 * @brief Run the held-key abort scenario.
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

    scene_init(&g_osys_null);
    video_init(&g_osys_null);
    palette_init(&g_osys_null);

    /* the idle prefix: the boot's fade to black and the first logo groups */
    InputState idle;
    memset(&idle, 0, sizeof idle);
    scene_note_input(&idle);

    int32_t r = 1;
    uint32_t t = 0;
    for (; t < 50u && r == 1; t++) {
        r = scene_play_boot_intro();
        palette_tick();
    }
    if (r != 1) {
        fprintf(stderr,
                "FAIL the boot finished during the 50-tick idle prefix\n");
        return 1;
    }

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
        held_ticks++;
    }

    if (r != 0) {
        fprintf(stderr,
                "FAIL the held key did not abort the boot (%u ticks, "
                "phase %u)\n",
                held_ticks, scene_boot_phase_for_probe());
        return 1;
    }
    if (!saw_abort) {
        fprintf(stderr, "FAIL the boot ended without taking BOOT_ABORT\n");
        return 1;
    }
    if (held_ticks > 300u) {
        fprintf(stderr, "FAIL the abort took %u ticks\n", held_ticks);
        return 1;
    }

    /* b60a: clear_fade_ramp + the 0x100-entry fade div 0x23 to black */
    {
        const uint8_t *pal = osys_null_palette();
        for (uint32_t i = 0; i < 768u; i++) {
            if (pal[i] != 0u) {
                fprintf(stderr,
                        "FAIL the abort fade left palette byte %u = %u\n", i,
                        pal[i]);
                return 1;
            }
        }
    }

    printf("ok D-245: a held key aborts the boot to the attract path\n");
    return 0;
}
