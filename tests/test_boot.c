/* test_boot.c — the per-tick boot-intro player against the real INTRO.RSC.
 *
 * The boot intro (load_intro_scene, 1000:B63B) plays INTRO.RSC's display
 * list over the scrolled bac.bit background with the tree/apple sprites.
 * The state machine: the 280-tick initial dwell, then 387 display-list
 * frames (2 ticks each), ending on the quit cue (frame 385).
 *
 * The page's exact bytes depend on the game's garbage-pointer behaviour
 * for the out-of-range table indices in the display list (e.g. the frame
 * 305 record's idx = 38711 — the game decodes the garbage pointer into
 * its segment; the port bounds-checks and skips).  The test therefore
 * asserts the state machine's shape (the tick count, the scrolled
 * background, the palette) rather than a full page checksum — a full
 * pixel oracle needs a DOSBox capture.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (expected bytes).
 * Missing volumes = skip. */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern const uint8_t *osys_null_page(int which);
extern const uint8_t *osys_null_palette(void);

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);

    char fix_abs[512];
    if (!compat_realpath(fix_dir, fix_abs)) {
        fprintf(stderr, "FAIL fixture tree %s unreadable\n", fix_dir);
        return 1;
    }

    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }

    int bad = 0;
    scene_init(&osys_null);
    video_init(&osys_null);

    /* 1. the intro plays until the quit cue, then reports done. */
    unsigned ticks = 0;
    int r;
    while ((r = scene_playBootIntro()) == 1) {
        if (++ticks > 4000u) break;        /* runaways are failures */
    }
    if (r != 0) {
        fprintf(stderr, "FAIL the boot intro did not finish (%u ticks)\n", ticks);
        return 1;
    }
    /* the expected shape: the 280-tick dwell + 387 frames x 3 calls
     * (1 render + 2 wait), ending at the quit cue (frame 385). */
    if (ticks < 1400u || ticks > 1500u) {
        fprintf(stderr, "FAIL tick count %u outside [1400, 1500]\n", ticks);
        bad = 1;
    }

    /* 2. the page: the scrolled bac screen covers rows 0..183 (58880
     * bytes, all written by the wrap copy); the sprites ride it and the
     * display-list overlays draw on top (they legitimately reach the
     * screen's bottom rows). */
    {
        const uint8_t *page = osys_null_page(0);
        unsigned nz = 0;
        for (unsigned i = 0; i < 58880u; i++) if (page[i]) nz++;
        if (nz < 58000u) {
            fprintf(stderr, "FAIL the scrolled background barely drawn (%u)\n", nz);
            bad = 1;
        }
    }

    /* 3. the palette is bac.bit's trailing 768 bytes (at +0xE600). */
    {
        uint32_t pal_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.000/INTRO/BAC.BIT", fix_abs);
        uint8_t *bac = read_file(path, &pal_size);
        if (!bac || pal_size < 0xE600u + 768u) {
            fprintf(stderr, "FAIL bac.bit fixture unreadable\n");
            bad = 1;
        } else if (memcmp(osys_null_palette(), bac + 0xE600u, 768u) != 0) {
            fprintf(stderr, "FAIL palette != bac.bit's 768-byte tail\n");
            bad = 1;
        }
        free(bac);
    }

    if (!bad) printf("boot tests: OK (3 checks)\n");
    return bad;
}
