/* test_scene.c — the per-tick level-intro player against the real ANI data.
 *
 * Level 0 -> disc 2 (the ported 0x6287 table) -> ANI00.ANI.  The intro is
 * driven one tick per scene_playLevelIntro call: 210-tick dwell, then 35
 * script entries (6 ticks each) AL-expanded onto page 0, then the chain
 * wait.  The final page must match a reference playback produced by the
 * validated python decoder (tools/ani_lzw.py) run over the same order on
 * top of the daily.bit screen + scoops background; the palette is
 * daily.bit's trailing 768 bytes.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (expected bytes).
 * Missing volumes = skip; a wrong decode with data present = fail. */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern void osys_null_inject(const InputState *in);
extern const uint8_t *osys_null_page(int which);
extern const uint8_t *osys_null_palette(void);

static unsigned xor16(const uint8_t *p, uint32_t n)
{
    unsigned v = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2)
        v ^= (unsigned)p[i] | ((unsigned)p[i + 1] << 8);
    return v;
}

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
    snprintf(path, sizeof path, "%s/HUMANS.002", vol_dir);
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
    palette_init(&osys_null);

    /* 1. the intro plays: dwell (210) + 34 objects (6 ticks each) — well
     * past that, every call still returns 1 (the scene-0 chain replays).
     * 34, not 35: the display list's last object is a tail-eater (its
     * stream runs to the loaded image's exact end) and the machine's
     * checksum gate rejects it — decode runs, the blit never does
     * (dosbox-staging debugger session, 2026-10-24: GT obj 29 fails the
     * b444 gate, EBP=0x613C; every scene file's last object is a
     * tail-eater, slack = 0 in all 12). */
    for (unsigned t = 0; t < 600; t++) {
        if (scene_playLevelIntro(0) != 1) {
            fprintf(stderr, "FAIL intro ended early at tick %u\n", t);
            return 1;
        }
    }

    /* 2. the page holds the final scene: byte-exact against the reference
     * playback (daily.bit screen + scoops bg + the 34 played AL expansions
     * over the zeroed page). */
    {
        const uint8_t *page = osys_null_page(0);
        unsigned x = xor16(page, 320u * 200u);
        unsigned sum = 0;
        for (unsigned i = 0; i < 320u * 200u; i++) sum += page[i];
        sum &= 0xFFFFu;
        if (x != 0xbb97u) {
            fprintf(stderr, "FAIL page xor16=%#06x, want 0xbb97\n", x);
            bad = 1;
        }
        if (sum != 0xfea4u) {
            fprintf(stderr, "FAIL page sum16=%#06x, want 0xfea4\n", sum);
            bad = 1;
        }
    }

    /* 3. the palette is daily.bit's trailing 768 bytes (6-bit RGB). */
    {
        uint32_t pal_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.002/LBM/DAILY.BIT", fix_abs);
        uint8_t *daily = read_file(path, &pal_size);
        if (!daily || pal_size < 64000u + 768u) {
            fprintf(stderr, "FAIL daily.bit fixture unreadable\n");
            bad = 1;
        } else if (memcmp(osys_null_palette(), daily + 64000u, 768u) != 0) {
            fprintf(stderr, "FAIL palette != daily.bit's 768-byte tail\n");
            bad = 1;
        }
        free(daily);
    }

    /* 4. a key quits the replaying intro. */
    {
        InputState k;
        memset(&k, 0, sizeof k);
        k.lastKey = 1;
        osys_null_inject(&k);
        if (scene_playLevelIntro(0) != 0) {
            fprintf(stderr, "FAIL key did not quit the intro\n");
            bad = 1;
        }
    }

    /* 5. after finishing, the same level stays finished. */
    if (scene_playLevelIntro(0) != 0) {
        fprintf(stderr, "FAIL finished intro replayed\n");
        bad = 1;
    }

    /* 6. an out-of-range level index reports no cutscene. */
    if (scene_playLevelIntro(0xFFu) != 0) {
        fprintf(stderr, "FAIL scene_playLevelIntro(0xFF) should return 0\n");
        bad = 1;
    }

    if (!bad) printf("scene tests: OK (5 checks)\n");
    return bad;
}
