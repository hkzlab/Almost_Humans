/* test_boot_flash.c - D-246: the frame-0 white flash runs over the scene.
 *
 * load_intro_scene (1000:b63b) renders and presents the scene's first frame
 * (1000:b71e-b739) before entering the frame-keyed cue loop.  The port used
 * to skip that block, so the frame-0 transition cue (1000:b82e) faded the
 * DAC to white while the page still held the ITB card: "In the beginning....."
 * flashed bright white.
 *
 * The test drives the boot with idle input to the first transition, waits
 * for the white DAC, and requires the visible rows to differ from the ITB
 * card.  The card's own palette fade-in/dwell/fade-out is not involved: the
 * stale page under the flash is the whole defect.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * Missing volumes = skip. */

#include "audio.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "scene.h"
#include "video.h"
#include "compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);
extern const OSystem g_osys_null;

/* BootPhase values (the private enum in scene.c): the first transition. */
enum {
    BOOT_TRANS_PHASE = 28
};

/**
 * @brief Run the frame-0 flash scenario.
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

    osys_bind(&g_osys_null);
    res_init();
    scene_init();
    video_init();
    palette_init();

    uint32_t itb_size = 0;
    uint8_t *itb = res_load_chunk("INTRO\\ITB.BIT", &itb_size, NULL);
    if (!itb || itb_size < 64000u) {
        fprintf(stderr, "FAIL intro\\itb.bit fixture unreadable\n");
        free(itb);
        return 1;
    }

    InputState idle;
    memset(&idle, 0, sizeof idle);
    scene_note_input(&idle);

    uint8_t r = 1;
    uint32_t t = 0;
    int32_t checked = 0;
    while (r == 1 && t < 8000u) {
        if (scene_boot_phase_for_probe() == BOOT_TRANS_PHASE &&
            !palette_fade_active()) {
            const uint8_t *page = osys_null_page(0);
            const uint8_t *pal = osys_null_palette();
            uint32_t diff = 64000u;

            /* 1000:b854-b860: the white ramp, BP=2 (entry 0 stays black) */
            if (pal[12] != 0x3Fu) {
                fprintf(stderr, "FAIL the flash DAC is not white (%u)\n",
                        pal[12]);
                free(itb);
                return 1;
            }
            for (uint32_t i = 0; i < 180u * 320u; i++) {
                if (page[i] != itb[i]) {
                    diff = i;
                    break;
                }
            }
            if (diff == 64000u) {
                fprintf(stderr,
                        "FAIL the white-flash page is still the ITB card "
                        "(visible rows identical)\n");
                free(itb);
                return 1;
            }
            printf("ok D-246: the frame-0 flash is over the scene (first "
                   "difference at byte %u)\n",
                   diff);
            checked = 1;
            break;
        }
        r = scene_play_boot_intro();
        palette_tick();
        audio_tick();
        t++;
    }

    free(itb);
    if (!checked) {
        fprintf(stderr,
                "FAIL the boot never reached the white transition "
                "(tick %u, phase %u, r %d)\n",
                t, scene_boot_phase_for_probe(), r);
        return 1;
    }
    return 0;
}
