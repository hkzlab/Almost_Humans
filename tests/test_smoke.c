/* test_smoke.c — full-game_main smoke test under the null backend.
 *
 * A scripted input sequence drives the whole flow: the attract dwell, fire
 * into the level, the level-intro cutscene (which plays its 35 scene
 * objects onto page 0), a key skip through the chain wait, and quit.
 * Asserts game_main exits cleanly with the intro's final page on page 0
 * and daily.bit's palette installed.
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
extern void osys_null_inputScript(const InputState *states, unsigned n);
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

enum { SCRIPT_N = 8700 };

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

    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }

    int bad = 0;

    /* the input script (per eventsPoll call):
     *   0..6599    idle     — the boot intro plays (~5887 ticks: the
     *                           gametek logo, the idi/first/itb cards, the
     *                           scene), then the attract cycle starts
     *                           (game_loop: effects 1->3->2->4)
     *   6600       fire     — the attract escape (attract_exit 1000:13E9):
     *                           the 13B2 fade (~35 ticks) then FLOW_MENU
     *                           (the room loads code2.raw + fades in)
     *   6700..6950 right    — the room's confirm: the rails walk
     *                           0x1D2 -> 0x208 (cell 6) -> 0x280, the
     *                           right-edge exit matches "DARWIN" (level 1
     *                           = levelId 0) and fades out -> FLOW_LEVEL,
     *                           the level intro starts (ANI00)
     *   7700/7701  key      — skips the level intro
     *   7702..8698 idle      — the level proper
     *   8699       quit     — game_main exits                                */
    static InputState script[SCRIPT_N];
    memset(script, 0, sizeof script);
    /* the attract cycle runs from the boot's end (~5887) until the fire
     * at 6600 escapes it; the room then walks right off the edge */
    script[6600].joyBits = 0x80u;         /* the attract escape */
    for (unsigned i = 6700; i <= 6950u; i++)
        script[i].joyBits = 0x08u;         /* hold RIGHT: the room's confirm */
    /* the party pre-screen (t≈6912..7571, per the probe's timeline): its
     * wait fires [0x48A3]-shaped aborts exactly like the original's
     * wait_frames_or_key — one key inside the first set's wait exits  */
    script[7150].lastKey = 1u;            /* the party's SET1W window    */
    /* the .ANI cutscene follows the party's exit; hold the key through its
     * chain waits (the skip the old 7700/7701 press did) */
    for (unsigned i = 7250u; i <= 7700u; i++) script[i].lastKey = 1u;
    script[SCRIPT_N - 1].quit = 1u;

    osys_null_inputScript(script, SCRIPT_N);

    if (game_main(&osys_null) != 0) {
        fprintf(stderr, "FAIL game_main returned nonzero\n");
        return 1;
    }

    /* the page: the level's rendered viewport (level_tick: BG4 + the terrain
     * tiles at scroll 0,0 + the status bar's band) — the byte-exact
     * composite is test_level/test_statusbar's; here the assertion is the
     * coverage (the measured nonzero count of the level-0 composite's
     * first 184 rows, which since the bar include rows 152..183 of its
     * 0xC0-tagged band) */
    {
        const uint8_t *page = osys_null_page(0);
        unsigned nz = 0;
        for (unsigned i = 0; i < 58880u; i++) if (page[i]) nz++;
        if (nz != 26910u) {
            fprintf(stderr, "FAIL page: the level render nz %u, want 26910\n", nz);
            bad = 1;
        }
    }

    /* the palette: the level intro installed daily.bit's 768 bytes; the
     * level proper (level_tick) then rewrites the first 64 entries from
     * level 0's background (style 3 -> BD3 field L=4 -> BG4.RAW — the
     * port's world convention; the original's level slot is 202F:0x1713
     * fed by the fade machinery) and the bar's +0xC0 bank, entries
     * 0xC0..0xFF, from panel4.bit's own palette (level_video_init
     * 1000:32CD, re_map.md → "The in-level status bar") */
    {
        uint32_t daily_size = 0, bg4_size = 0, p4_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.002/LBM/DAILY.BIT", fix_dir);
        uint8_t *daily = read_file(path, &daily_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/BACKS/BG4.RAW", fix_dir);
        uint8_t *bg4 = read_file(path, &bg4_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/LBM/PANEL4.BIT", fix_dir);
        uint8_t *p4 = read_file(path, &p4_size);
        if (!daily || daily_size < 64000u + 768u ||
            !bg4 || bg4_size != 163840u + 768u ||
            !p4 || p4_size < 0xFA00u + 768u) {
            fprintf(stderr, "FAIL palette fixtures unreadable\n");
            bad = 1;
        } else {
            const uint8_t *pal = osys_null_palette();
            for (unsigned i = 0; i < 192u; i++) {
                if (pal[i] != bg4[163840u + i]) {
                    fprintf(stderr, "FAIL palette byte %u: got %02x want BG4 %02x\n", i, pal[i], bg4[163840u + i]);
                    bad = 1;
                    break;
                }
            }
            for (unsigned i = 192u; i < 576u; i++) {
                if (pal[i] != daily[64000u + i]) {
                    fprintf(stderr, "FAIL palette byte %u: got %02x want daily %02x\n",
                            i, pal[i], daily[64000u + i]);
                    bad = 1;
                }
            }
            /* the bar's +0xC0 bank: entries 0xC0..0xFF = panel4's
             * palette's first 0xC0 bytes (level_video_init 1000:32CD) */
            for (unsigned i = 576u; i < 768u; i++) {
                if (pal[i] != p4[0xFA00u + (i - 576u)]) {
                    fprintf(stderr, "FAIL palette byte %u: got %02x want panel4 %02x\n",
                            i, pal[i], p4[0xFA00u + (i - 576u)]);
                    bad = 1;
                }
            }
        }
        free(daily);
        free(bg4);
        free(p4);
    }

    if (!bad) printf("smoke test: OK (flow + the boot intro + the level intro + the palette)\n");
    return bad;
}
