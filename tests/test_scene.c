/* test_scene.c — the per-tick level-intro player against the real ANI data.
 *
 * Level 0 -> disc 2 (the ported 0x6287 table) -> ANI00.ANI.  The intro is
 * driven one tick per scene_play_level_intro call: 210-tick dwell, then 35
 * script entries (6 ticks each) AL-expanded onto page 0, then the chain
 * wait.  The final page must match a reference playback produced by the
 * validated python decoder (tools/ani_lzw.py) run over the same order on
 * top of the daily.bit screen + scoops background; the palette is
 * daily.bit's trailing 768 bytes.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (expected bytes).
 * Missing volumes = skip; a wrong decode with data present = fail. */
#include "osystem.h"
#include "palette.h"
#include "scene.h"
#include "video.h"
#include "compat.h"
#include "byteorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void osys_null_inject(const InputState *in);
extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);

extern const OSystem g_osys_null;

/**
 * @brief XOR the little-endian words of a byte range.
 *
 * @param p the byte range
 * @param n the count
 * @return the XOR of the words
 */
static uint32_t _xor16(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2)
        v ^= (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8);
    return v;
}

/**
 * @brief Read a whole file into a malloc'd buffer; NULL on error.
 *
 * @param path the path
 * @param size the size
 * @return the pointer, or NULL when unavailable
 */
static uint8_t *_read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int64_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}

/** @brief Drive an intro to its first played frame: the panel's 0x20-entry fade-in,
 * the 210-tick dwell and the 0x20-entry black fade all run before the
 * prologue frame lands, and every fade is stepped by palette_tick (the
 * ISR's).  Returns 1 once the page has changed from the panel.
 *
 * @param idx the index
 * @return 0 when the first frame was reached, 1 otherwise
 */
static int32_t _scene_to_first_frame(uint32_t idx)
{
    static uint8_t panel[64000];
    if (scene_play_level_intro(idx) != 1) return 0;
    memcpy(panel, osys_null_page(0), sizeof panel);
    for (uint32_t t = 0; t < 400u; t++) {
        palette_tick();
        if (scene_play_level_intro(idx) != 1) return 0;
        if (memcmp(panel, osys_null_page(0), sizeof panel) != 0) return 1;
    }
    return 0;
}

/**
 * @brief Run the intro scene tests.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
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

    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    int32_t bad = 0;
    scene_init(&g_osys_null);
    video_init(&g_osys_null);
    palette_init(&g_osys_null);

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
        uint8_t *df = _read_file(path, &psz);
        if (df && psz >= 64000u + 768u) {
            if (scene_play_level_intro(0) != 1) {
                fprintf(stderr, "FAIL the scene did not start\n");
                bad = 1;
            } else if (!palette_fade_active()) {
                fprintf(stderr, "FAIL the panel fade-in did not start\n");
                bad = 1;
            } else {
                uint32_t t = 0;
                while (palette_fade_active() && t < 100u) {
                    palette_tick();
                    t++;
                }
                const uint8_t *pal = osys_null_palette();
                if (palette_fade_active()) {
                    fprintf(stderr, "FAIL the panel fade-in never finished\n");
                    bad = 1;
                } else if (memcmp(pal, df + 64000u, 0x60u) != 0) {
                    fprintf(stderr, "FAIL the panel's first 0x20 entries are "
                                    "not daily.bit's after the fade\n");
                    bad = 1;
                } else {
                    for (uint32_t i = 0x60u; i < 768u; i++)
                        if (pal[i]) {
                            fprintf(stderr,
                                    "FAIL the panel fade touched "
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
        uint8_t *daily = _read_file(path, &dailySize);
        snprintf(path, sizeof path, "%s/HUMANS.002/APL/SCOOPS.APL", fix_abs);
        uint8_t *scoop = _read_file(path, &scoopSize);
        if (scene_play_level_intro(0) != 1) {
            fprintf(stderr, "FAIL the scene did not start\n");
            bad = 1;
        } else if (!daily || dailySize < 64000u || !scoop ||
                   scoopSize < 0x2000u) {
            fprintf(stderr, "FAIL panel fixtures unreadable\n");
            bad = 1;
        } else {
            static const uint8_t bg[10] = { 1, 2, 3, 4, 5, 6, 8, 0, 5, 7 };
            const uint8_t *src = scoop + 0x400u + (uint32_t)bg[0] * 0xE00u;
            const uint8_t *page = osys_null_page(0);
            uint32_t wrong = 0;
            for (uint32_t row = 0; row < 0x38u; row++)
                for (uint32_t col = 0; col < 0x40u; col++)
                    if (page[(104u + row) * 320u + 112u + col] !=
                        src[row * 0x40u + col])
                        wrong++;
            if (wrong) {
                fprintf(stderr,
                        "FAIL the scoops thumbnail is not a %ux%u "
                        "block at (112,104): %u bytes off\n",
                        64u, 56u, wrong);
                bad = 1;
            }
            /* the panel's headline (1000:B0D4: blit_text_tile, centre
             * 0xA0, row 0x41, mask 0x10) — bit 4 forced over the paper,
             * i.e. `daily pixel | 0x10`, inside the line's 0x45..0x4E band */
            {
                uint32_t ink = 0;
                for (uint32_t y = 0x45u; y <= 0x4Eu; y++)
                    for (uint32_t x = 0; x < 320u; x++) {
                        uint32_t at = y * 320u + x;
                        if (page[at] == (uint8_t)(daily[at] | 0x10u) &&
                            page[at] != daily[at])
                            ink++;
                    }
                if (ink < 20u) {
                    fprintf(stderr,
                            "FAIL the panel's headline is not drawn "
                            "(%u glyph pixels)\n",
                            ink);
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
        uint32_t guard = 0;
        while (!palette_fade_active() && guard < 300u) {
            palette_tick();
            if (scene_play_level_intro(0) != 1) {
                fprintf(stderr, "FAIL intro ended before the panel "
                                "fade-out\n");
                return 1;
            }
            guard++;
        }
        if (!palette_fade_active()) {
            fprintf(stderr, "FAIL the panel fade-out to black never started\n");
            bad = 1;
        } else {
            uint32_t t = 0;
            while (palette_fade_active() && t < 100u) {
                palette_tick();
                t++;
            }
            const uint8_t *pal = osys_null_palette();
            for (uint32_t i = 0; i < 0x60u; i++)
                if (pal[i]) {
                    fprintf(stderr,
                            "FAIL the panel fade-out did not reach "
                            "black (byte %u = %u)\n",
                            i, pal[i]);
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
        uint32_t guard = 0;
        while (memcmp(panelPage, osys_null_page(0), sizeof panelPage) == 0 &&
               guard < 400u) {
            palette_tick();
            if (scene_play_level_intro(0) != 1) {
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
        } else if (!palette_fade_active()) {
            fprintf(stderr, "FAIL the script palette fade did not start with "
                            "the first frame\n");
            bad = 1;
        }
    }

    /* the rest of the palette fade to the script object's palette
     * (1000:B263, div 0x19) + the 34 objects (6 ticks each) end inside the
     * chain wait, whose 210 ticks neither redraw nor touch the DAC.  Run
     * until the page has been quiet for 60 ticks (inside that wait: the
     * objects are 6 ticks apart), so the frame pinned below is the last one
     * whether or not the chain then replays the scene. */
    {
        uint32_t last = 0;
        uint32_t quiet = 0;

        for (uint32_t t = 1; t < 3000u && quiet < 60u; t++) {
            uint32_t h = 0;

            palette_tick();             /* the ISR body's fade step */
            if (scene_play_level_intro(0) != 1) {
                fprintf(stderr, "FAIL intro ended early at tick %u\n", t);
                return 1;
            }
            for (uint32_t i = 0; i + 1 < 320u * 200u; i += 2)
                h ^= (uint32_t)(rd_le_u16(osys_null_page(0) + i));
            if (h == last) quiet++;
            else {
                last = h;
                quiet = 0;
            }
        }
    }

    /* 2. the page holds the final scene: byte-exact against the reference
     * playback (daily.bit screen + scoops bg + the 34 played AL expansions
     * over the zeroed page). */
    {
        const uint8_t *page = osys_null_page(0);
        uint32_t x = _xor16(page, 320u * 200u);
        uint32_t sum = 0;
        for (uint32_t i = 0; i < 320u * 200u; i++) sum += page[i];
        sum &= 0xFFFFu;
        if (x != 0xbb97u) {
            fprintf(stderr, "FAIL page _xor16=%#06x, want 0xbb97\n", x);
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
        uint8_t *daily = _read_file(path, &pal_size);
        if (!daily || pal_size < 64000u + 768u) {
            fprintf(stderr, "FAIL daily.bit fixture unreadable\n");
            bad = 1;
        } else {
            const uint8_t *pal = osys_null_palette();
            if (memcmp(pal, daily + 64000u, 768u) == 0) {
                fprintf(stderr,
                        "FAIL palette is still daily.bit's tail "
                        "after the frames (1000:B263's fade did not run)\n");
                bad = 1;
            }
            for (uint32_t i = 0; i < 768u; i++) {
                if (pal[i] > 0x3Fu) {
                    fprintf(stderr, "FAIL palette byte %u = %#04x > 0x3f\n", i,
                            pal[i]);
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
        if (scene_play_level_intro(0) != 1) {
            fprintf(stderr, "FAIL the key did not start the fade-out\n");
            bad = 1;
        } else {
            uint32_t t = 0;
            int32_t r = 1;
            while (r == 1 && t < 100u) {
                palette_tick();
                r = scene_play_level_intro(0);
                t++;
            }
            if (r != 0) {
                fprintf(stderr, "FAIL key did not quit the intro\n");
                bad = 1;
            } else {
                const uint8_t *pal = osys_null_palette();
                for (uint32_t i = 0; i < 768u; i++)
                    if (pal[i]) {
                        fprintf(stderr,
                                "FAIL the quit fade did not reach "
                                "black (byte %u = %u)\n",
                                i, pal[i]);
                        bad = 1;
                        break;
                    }
            }
        }
    }

    /* 5. after finishing, the same level stays finished. */
    if (scene_play_level_intro(0) != 0) {
        fprintf(stderr, "FAIL finished intro replayed\n");
        bad = 1;
    }

    /* 5b. a new cutscene with the same index replays after a reset: cd_file_op
     * reloads every time, while the per-tick player keeps a finished intro
     * finished (test 5) */
    {
        InputState idle;
        memset(&idle, 0, sizeof idle);
        osys_null_inject(&idle);
        scene_reset_intro();
        if (scene_play_level_intro(0) != 1) {
            fprintf(stderr, "FAIL a reset did not restart the cutscene\n");
            bad = 1;
        }
        {
            InputState k;
            memset(&k, 0, sizeof k);
            k.lastKey = 1;
            osys_null_inject(&k);
            (void)scene_play_level_intro(0);
        }
        osys_null_inject(&idle);
    }

    /* 6b. [0x627C] (the boot's fast mode): play_scene_sequence's pre-frame
     * palette fade (1000:B263) is 4 entries div 2 in fast mode and 0x100
     * div 0x19 otherwise, and wait_flag_timeout (1000:B2D3) waits 1 tick per
     * object instead of 6.  Both are measured in *ticks*, so the scenes'
     * palettes do not matter: index 1 runs fast, index 2 slow. */
    {
        uint32_t fadeTicks[2], waitTicks[2];
        /* test 4 injected a held key and the null backend keeps handing it
         * back: clear it or every later scene skips its dwell at once */
        InputState idle;
        memset(&idle, 0, sizeof idle);
        osys_null_inject(&idle);
        for (uint32_t k = 0; k < 2u; k++) {
            uint32_t idx = k ? 2u : 1u;
            scene_set_fast_mode(k ? 0 : 1);
            if (!_scene_to_first_frame(idx)) {
                fprintf(stderr, "FAIL scene %u never reached its frames\n",
                        idx);
                bad = 1;
                continue;
            }
            uint32_t t = 0;
            while (palette_fade_active() && t < 100u) {
                palette_tick();
                t++;
            }
            fadeTicks[k] = t;
            /* one object's wait: the page moves when the next frame lands.
             * The call after the fade plays object 0 (it sets objWait), so
             * consume it first: the 0->1 frame boundary is not a wait. */
            (void)scene_play_level_intro(idx);
            {
                static uint8_t before[64000], after[64000];
                memcpy(before, osys_null_page(0), sizeof before);
                uint32_t w = 0;
                for (; w < 12u; w++) {
                    (void)scene_play_level_intro(idx);
                    memcpy(after, osys_null_page(0), sizeof after);
                    if (memcmp(before, after, sizeof before) != 0) {
                        w++;
                        break;
                    }
                }
                waitTicks[k] = w;
            }
        }
        scene_set_fast_mode(0);
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
    if (scene_play_level_intro(0xFFu) != 0) {
        fprintf(stderr, "FAIL scene_play_level_intro(0xFF) should return 0\n");
        bad = 1;
    }

    /* 7. the ending scene loops until a key.  Scene 6 loses
     * (HUMANS FAIL IN RACE FOR EVOLUTION) and its chain entry is itself
     *(202F:0x6293[6] = 6): 1000:B17E's self case waits 210 ticks
     *(1000:B1A6 - also the key/fire poll) and then replays from the first
     * object (1000:B189 -> 0xB14D -> 0xB211).  With no key the page must
     * start changing again after that quiet wait; a key then quits. */
    {
        InputState idle, key;
        uint32_t last = 0;
        uint32_t quiet = 0, replayed = 0, t;

        memset(&idle, 0, sizeof idle);
        memset(&key, 0, sizeof key);
        key.lastKey = 1;
        osys_null_inject(&idle);
        scene_reset_intro();
        if (scene_play_level_intro(6) != 1) {
            fprintf(stderr, "FAIL the ending scene did not start\n");
            bad = 1;
        } else {
            uint32_t h = 0;

            for (t = 0; t < 3000u; t++) {
                palette_tick();
                if (scene_play_level_intro(6) != 1) break;
                h = 0;
                for (uint32_t i = 0; i + 1 < 320u * 200u; i += 2)
                    h ^= (uint32_t)(rd_le_u16(osys_null_page(0) + i));
                if (h == last) {
                    quiet++;
                } else {
                    if (quiet >= 200u) replayed = 1;   /* after the wait */
                    last = h;
                    quiet = 0;
                }
            }
            if (!replayed) {
                fprintf(stderr, "FAIL the ending did not replay after the "
                                "210-tick chain wait\n");
                bad = 1;
            }
            osys_null_inject(&key);
            if (scene_play_level_intro(6) != 1) {
                fprintf(stderr, "FAIL the key did not start the quit fade\n");
                bad = 1;
            } else {
                int32_t r = 1;
                for (t = 0; t < 200u && r == 1; t++) {
                    palette_tick();
                    r = scene_play_level_intro(6);
                }
                if (r != 0) {
                    fprintf(stderr, "FAIL the key did not end the ending\n");
                    bad = 1;
                }
            }
            osys_null_inject(&idle);
        }
    }

    /* D-105 (closed 2026-11-05): the headline block's string 9 at
     * 202F:0x6416 is EMPTY.  1000:B0C1 walks to the (index+1)-th NUL, so
     * scene 8 draws no headline and scene 9 draws "HUMANS RECOVER THE STOLEN
     * IDOL!".  Both ANIs ship (index 8 -> disc 2, index 9 -> disc 2). */
    {
        uint32_t dsz = 0;
        snprintf(path, sizeof path, "%s/HUMANS.002/LBM/DAILY.BIT", fix_abs);
        uint8_t *daily = _read_file(path, &dsz);
        if (!daily || dsz < 64000u) {
            fprintf(stderr, "FAIL D-105: DAILY.BIT unreadable\n");
            bad = 1;
        } else {
            static const uint32_t idx[2] = { 8u, 9u };
            static const uint32_t want_ink[2] = { 0u, 1u };

            for (uint32_t k = 0; k < 2u; k++) {
                scene_reset_intro();
                if (scene_play_level_intro(idx[k]) != 1) {
                    fprintf(stderr, "FAIL D-105: scene %u did not start\n",
                            idx[k]);
                    bad = 1;
                    continue;
                }
                {
                    const uint8_t *page = osys_null_page(0);
                    uint32_t ink = 0;

                    for (uint32_t y = 0x45u; y <= 0x4Eu; y++)
                        for (uint32_t x = 0; x < 320u; x++) {
                            uint32_t at = y * 320u + x;

                            if (page[at] == (uint8_t)(daily[at] | 0x10u) &&
                                page[at] != daily[at])
                                ink++;
                        }
                    if ((ink != 0u) != (want_ink[k] != 0u)) {
                        fprintf(stderr,
                                "FAIL D-105: scene %u headline ink %u "
                                "(want %s)\n",
                                idx[k], ink, want_ink[k] ? "drawn" : "blank");
                        bad = 1;
                    }
                }
            }
            /* the ending scene (6) the previous block left behind */
            scene_reset_intro();
        }
        free(daily);
    }

    if (!bad) printf("scene tests: OK (7 checks)\n");
    return bad;
}
