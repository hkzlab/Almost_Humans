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

/* Drive an intro to its first played frame: the panel's 0x20-entry fade-in,
 * the 210-tick dwell and the 0x20-entry black fade all run before the
 * prologue frame lands, and every fade is stepped by palette_tick (the
 * ISR's).  Returns 1 once the page has changed from the panel. */
static int scene_to_firstFrame(unsigned idx)
{
    static uint8_t panel[64000];
    if (scene_playLevelIntro(idx) != 1) return 0;
    memcpy(panel, osys_null_page(0), sizeof panel);
    for (unsigned t = 0; t < 400u; t++) {
        palette_tick();
        if (scene_playLevelIntro(idx) != 1) return 0;
        if (memcmp(panel, osys_null_page(0), sizeof panel) != 0) return 1;
    }
    return 0;
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
    /* 1000:B146: the panel's first 0x20 DAC entries FADE from the previous
     * screen's palette to daily.bit's over div 0x19; the rest of the DAC is
     * untouched, and the 210-tick dwell starts only after the fade. */
    {
        snprintf(path, sizeof path, "%s/HUMANS.002/LBM/DAILY.BIT", fix_abs);
        uint32_t psz = 0;
        uint8_t *df = read_file(path, &psz);
        if (df && psz >= 64000u + 768u) {
            if (scene_playLevelIntro(0) != 1) {
                fprintf(stderr, "FAIL the scene did not start\n");
                bad = 1;
            } else if (!palette_fadeActive()) {
                fprintf(stderr, "FAIL the panel fade-in did not start\n");
                bad = 1;
            } else {
                unsigned t = 0;
                while (palette_fadeActive() && t < 100u) {
                    palette_tick();
                    t++;
                }
                const uint8_t *pal = osys_null_palette();
                if (palette_fadeActive()) {
                    fprintf(stderr, "FAIL the panel fade-in never finished\n");
                    bad = 1;
                } else if (memcmp(pal, df + 64000u, 0x60u) != 0) {
                    fprintf(stderr, "FAIL the panel's first 0x20 entries are "
                            "not daily.bit's after the fade\n");
                    bad = 1;
                } else {
                    for (unsigned i = 0x60u; i < 768u; i++)
                        if (pal[i]) {
                            fprintf(stderr, "FAIL the panel fade touched "
                                    "entry %#x (it stops at 0x1f)\n",
                                    i / 3u);
                            bad = 1;
                            break;
                        }
                }
            }
        }
        free(df);
    }

    /* 0. the first call composes the loading panel: daily.bit (the
     * newspaper) with the scoops.apl thumbnail overlaid.  1000:B1CF places
     * it at row 104, column 112 of the 320-wide panel, 56 rows of 64 bytes,
     * the row step being REP MOVSB's 0x40 plus the following `ADD DI,0x100`
     * = 0x140.  Taking only the 0x100 shears the thumbnail 64 px per row,
     * which is exactly the corrupted panel the port used to show. */
    {
        uint32_t dailySize = 0, scoopSize = 0;
        snprintf(path, sizeof path, "%s/HUMANS.002/LBM/DAILY.BIT", fix_abs);
        uint8_t *daily = read_file(path, &dailySize);
        snprintf(path, sizeof path, "%s/HUMANS.002/APL/SCOOPS.APL", fix_abs);
        uint8_t *scoop = read_file(path, &scoopSize);
        if (scene_playLevelIntro(0) != 1) {
            fprintf(stderr, "FAIL the scene did not start\n");
            bad = 1;
        } else if (!daily || dailySize < 64000u || !scoop || scoopSize < 0x2000u) {
            fprintf(stderr, "FAIL panel fixtures unreadable\n");
            bad = 1;
        } else {
            static const uint8_t bg[10] = { 1,2,3,4,5,6,8,0,5,7 };
            const uint8_t *src = scoop + 0x400u + (uint32_t)bg[0] * 0xE00u;
            const uint8_t *page = osys_null_page(0);
            unsigned wrong = 0;
            for (unsigned row = 0; row < 0x38u; row++)
                for (unsigned col = 0; col < 0x40u; col++)
                    if (page[(104u + row) * 320u + 112u + col] !=
                        src[row * 0x40u + col])
                        wrong++;
            if (wrong) {
                fprintf(stderr, "FAIL the scoops thumbnail is not a %ux%u "
                        "block at (112,104): %u bytes off\n", 64u, 56u, wrong);
                bad = 1;
            }
            /* the panel's headline (1000:B0D4: blit_text_tile, centre
             * 0xA0, row 0x41, colour 0x10) — black glyph pixels inside
             * the line's 0x45..0x4E band */
            {
                unsigned ink = 0;
                for (unsigned y = 0x45u; y <= 0x4Eu; y++)
                    for (unsigned x = 0; x < 320u; x++)
                        if (page[y * 320u + x] == 0x10u) ink++;
                if (!ink) {
                    fprintf(stderr, "FAIL the panel's headline is not drawn\n");
                    bad = 1;
                }
            }

            /* the newspaper's own row-104 pixels outside the block: the
             * panel, not the block (a stride slip would reach them) */
            if (page[104u * 320u + 100u] != daily[104u * 320u + 100u]) {
                fprintf(stderr, "FAIL the panel's row 104 was overwritten "
                        "left of the thumbnail\n");
                bad = 1;
            }
        }
        free(daily);
        free(scoop);
    }

    /* 1000:B13F: at the end of the dwell the panel's first 0x20 entries fade
     * back to the zeroed [0x48BC] ramp (black) before play_scene_sequence
     * draws the first frame.  A fade that stopped short, or one that touched
     * other entries, would leave the wrong newspaper colours under the
     * frame. */
    {
        unsigned guard = 0;
        while (!palette_fadeActive() && guard < 300u) {
            palette_tick();
            if (scene_playLevelIntro(0) != 1) {
                fprintf(stderr, "FAIL intro ended before the panel "
                        "fade-out\n");
                return 1;
            }
            guard++;
        }
        if (!palette_fadeActive()) {
            fprintf(stderr, "FAIL the panel fade-out to black never started\n");
            bad = 1;
        } else {
            unsigned t = 0;
            while (palette_fadeActive() && t < 100u) {
                palette_tick();
                t++;
            }
            const uint8_t *pal = osys_null_palette();
            for (unsigned i = 0; i < 0x60u; i++)
                if (pal[i]) {
                    fprintf(stderr, "FAIL the panel fade-out did not reach "
                            "black (byte %u = %u)\n", i, pal[i]);
                    bad = 1;
                    break;
                }
        }
    }

    /* 1000:B23E..B262: the FIRST frame is decoded, drawn and flipped BEFORE
     * the palette fade, so the fade runs over the scene frame, not over the
     * panel (our bug: the newspaper stayed on screen and changed colours
     * while the DAC faded).  At the moment the fade starts the page must
     * already be the frame. */
    {
        static uint8_t panelPage[64000];
        memcpy(panelPage, osys_null_page(0), sizeof panelPage);
        unsigned guard = 0;
        while (memcmp(panelPage, osys_null_page(0), sizeof panelPage) == 0 &&
               guard < 400u) {
            palette_tick();
            if (scene_playLevelIntro(0) != 1) {
                fprintf(stderr, "FAIL intro ended while waiting for the "
                        "first frame\n");
                return 1;
            }
            guard++;
        }
        if (memcmp(panelPage, osys_null_page(0), sizeof panelPage) == 0) {
            fprintf(stderr, "FAIL the first frame is not drawn before the "
                    "palette fade (the panel is still on screen)\n");
            bad = 1;
        } else if (!palette_fadeActive()) {
            fprintf(stderr, "FAIL the script palette fade did not start with "
                    "the first frame\n");
            bad = 1;
        }
    }

    /* the rest of the palette fade to the script object's palette
     * (1000:B263, div 0x19) + the 34 objects (6 ticks each) end inside the
     * chain wait, whose 210 ticks neither redraw nor touch the DAC: at 500
     * ticks the page is the last frame and the DAC is the script palette. */
    for (unsigned t = 1; t < 500; t++) {
        palette_tick();                 /* the ISR body's fade step */
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

    /* 3. the DAC is the last played scene object's own 768-byte palette
     * (each object carries one after its u16 count) — 6-bit, and NOT the
     * panel's daily.bit palette (which used to stay installed). */
    {
        uint32_t pal_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.002/LBM/DAILY.BIT", fix_abs);
        uint8_t *daily = read_file(path, &pal_size);
        if (!daily || pal_size < 64000u + 768u) {
            fprintf(stderr, "FAIL daily.bit fixture unreadable\n");
            bad = 1;
        } else {
            const uint8_t *pal = osys_null_palette();
            if (memcmp(pal, daily + 64000u, 768u) == 0) {
                fprintf(stderr, "FAIL palette is still daily.bit's tail "
                        "after the frames (1000:B263's fade did not run)\n");
                bad = 1;
            }
            for (unsigned i = 0; i < 768u; i++) {
                if (pal[i] > 0x3Fu) {
                    fprintf(stderr, "FAIL palette byte %u = %#04x > 0x3f\n",
                            i, pal[i]);
                    bad = 1;
                    break;
                }
            }
        }
        free(daily);
    }

    /* 4. a key quits the replaying intro: 1000:B163 fades all 256 entries
     * to black before cd_file_op returns. */
    {
        InputState k;
        memset(&k, 0, sizeof k);
        k.lastKey = 1;
        osys_null_inject(&k);
        if (scene_playLevelIntro(0) != 1) {
            fprintf(stderr, "FAIL the key did not start the fade-out\n");
            bad = 1;
        } else {
            unsigned t = 0;
            int r = 1;
            while (r == 1 && t < 100u) {
                palette_tick();
                r = scene_playLevelIntro(0);
                t++;
            }
            if (r != 0) {
                fprintf(stderr, "FAIL key did not quit the intro\n");
                bad = 1;
            } else {
                const uint8_t *pal = osys_null_palette();
                for (unsigned i = 0; i < 768u; i++)
                    if (pal[i]) {
                        fprintf(stderr, "FAIL the quit fade did not reach "
                                "black (byte %u = %u)\n", i, pal[i]);
                        bad = 1;
                        break;
                    }
            }
        }
    }

    /* 5. after finishing, the same level stays finished. */
    if (scene_playLevelIntro(0) != 0) {
        fprintf(stderr, "FAIL finished intro replayed\n");
        bad = 1;
    }

    /* 6b. [0x627C] (the boot's fast mode): play_scene_sequence's pre-frame
     * palette fade (1000:B263) is 4 entries div 2 in fast mode and 0x100
     * div 0x19 otherwise, and wait_flag_timeout (1000:B2D3) waits 1 tick per
     * object instead of 6.  Both are measured in *ticks*, so the scenes'
     * palettes do not matter: index 1 runs fast, index 2 slow. */
    {
        unsigned fadeTicks[2], waitTicks[2];
        /* test 4 injected a held key and the null backend keeps handing it
         * back: clear it or every later scene skips its dwell at once */
        InputState idle;
        memset(&idle, 0, sizeof idle);
        osys_null_inject(&idle);
        for (unsigned k = 0; k < 2u; k++) {
            unsigned idx = k ? 2u : 1u;
            scene_setFastMode(k ? 0 : 1);
            if (!scene_to_firstFrame(idx)) {
                fprintf(stderr, "FAIL scene %u never reached its frames\n", idx);
                bad = 1;
                continue;
            }
            unsigned t = 0;
            while (palette_fadeActive() && t < 100u) { palette_tick(); t++; }
            fadeTicks[k] = t;
            /* one object's wait: the page moves when the next frame lands.
             * The call after the fade plays object 0 (it sets objWait), so
             * consume it first: the 0->1 frame boundary is not a wait. */
            (void)scene_playLevelIntro(idx);
            {
                static uint8_t before[64000], after[64000];
                memcpy(before, osys_null_page(0), sizeof before);
                unsigned w = 0;
                for (; w < 12u; w++) {
                    (void)scene_playLevelIntro(idx);
                    memcpy(after, osys_null_page(0), sizeof after);
                    if (memcmp(before, after, sizeof before) != 0) { w++; break; }
                }
                waitTicks[k] = w;
            }
        }
        scene_setFastMode(0);
        /* measured: the fade 3 vs 27 ticks, one object's wait 2 vs 7 (the
         * 1-vs-6 wait is the asm's; the extra tick is the flip boundary) */
        if (!(fadeTicks[0] + 6u < fadeTicks[1])) {
            fprintf(stderr, "FAIL the fast fade took %u ticks, the slow %u\n",
                    fadeTicks[0], fadeTicks[1]);
            bad = 1;
        }
        if (!(waitTicks[0] + 2u < waitTicks[1])) {
            fprintf(stderr, "FAIL the fast object wait %u ticks, the slow %u\n",
                    waitTicks[0], waitTicks[1]);
            bad = 1;
        }
    }

    /* 6. an out-of-range level index reports no cutscene. */
    if (scene_playLevelIntro(0xFFu) != 0) {
        fprintf(stderr, "FAIL scene_playLevelIntro(0xFF) should return 0\n");
        bad = 1;
    }

    if (!bad) printf("scene tests: OK (5 checks)\n");
    return bad;
}
