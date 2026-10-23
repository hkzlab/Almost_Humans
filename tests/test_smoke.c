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
#include <unistd.h>

extern const OSystem osys_null;
extern void osys_null_inputScript(const InputState *states, unsigned n);
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

enum { SCRIPT_N = 1500 };

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

    if (chdir(vol_dir)) { perror(vol_dir); return 1; }

    int bad = 0;

    /* the input script (per eventsPoll call):
     *   0..349    idle      — the attract dwell (350 iterations)
     *   350       fire      — menu confirm: FLOW_LEVEL, the intro starts
     *   351..1249 idle      — the intro (2 polls per iteration in LEVEL:
     *                         game_main's flow poll + scene.c's key poll):
     *                         210-tick dwell + 35 objects x 6 ticks
     *   1250..1251 key      — skip through the chain wait
     *   1252..1498 idle      — the level proper (no content yet)
     *   1499      quit      — game_main exits                              */
    static InputState script[SCRIPT_N];
    memset(script, 0, sizeof script);
    script[350].joyBits = 0x80u;          /* fire */
    script[1250].lastKey = 1u;            /* any key: skip the intro */
    script[1251].lastKey = 1u;
    script[SCRIPT_N - 1].quit = 1u;

    osys_null_inputScript(script, SCRIPT_N);

    if (game_main(&osys_null) != 0) {
        fprintf(stderr, "FAIL game_main returned nonzero\n");
        return 1;
    }

    /* the intro's final page is on page 0 */
    {
        const uint8_t *page = osys_null_page(0);
        unsigned x = xor16(page, 320u * 200u);
        unsigned sum = 0;
        for (unsigned i = 0; i < 320u * 200u; i++) sum += page[i];
        sum &= 0xFFFFu;
        if (x != 0xb992u) {
            fprintf(stderr, "FAIL page xor16=%#06x, want 0xb992\n", x);
            bad = 1;
        }
        if (sum != 0xfec1u) {
            fprintf(stderr, "FAIL page sum16=%#06x, want 0xfec1\n", sum);
            bad = 1;
        }
    }

    /* the palette is daily.bit's trailing 768 bytes */
    {
        uint32_t pal_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.002/LBM/DAILY.BIT", fix_dir);
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

    if (!bad) printf("smoke test: OK (flow + intro + page + palette)\n");
    return bad;
}
