/* test_statusbar.c — the in-level status bar against the shipped assets.
 *
 * The bar's page composition is checked against an independent model built
 * straight from the fixture files (PANELN1.SPR + PANELN2.SPR = the glyph
 * strip, PANEL4.BIT = the band art): the band (panel4's top 40 rows,
 * +0xC0-tagged) and the five digit marks (the countdown's minutes /
 * sec-tens / sec-ones + the tribe pair's tens / ones, each an 8x8 glyph at
 * the (digit+1)*16 strip slot, re_map.md → "The in-level status bar,
 * decoded end to end").
 *
 * Cases: the seeded stamp; the first per-second roll (6:00 -> 5:59, the
 * odometer's one-frame roll-start phase, then the settled glyph); the
 * tribe odometer's roll-down; the time-up freeze; and the level_load +
 * level_tick integration (level 0's own timeA and its map's 0xAA count).
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (expected bytes).
 * argv[3] = optional PPM dump directory (the level page at t=0 and t=90,
 *           rendered with the installed DAC — for eyeballing/analysis).
 * Missing volumes = skip. */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern const uint8_t *osys_null_page(int which);
extern const uint8_t *osys_null_palette(void);

#define BAR_Y 152u
#define MARK_Y 172u
#define MARK_X(m) ((const uint16_t[5]){ 8, 24, 32, 288, 296 })[m]
#define MARK_BIAS(m) ((const uint16_t[5]){ 0, 0xC0, 0, 0, 0 })[m]

static uint8_t strip[1280];
static uint8_t panel4[12800 + 0xFA00 + 768]; /* art + the palette */

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

static int load_fixtures(const char *fix_dir)
{
    char path[512];
    uint32_t n1s = 0, n2s = 0, p4s = 0;
    uint8_t *n1, *n2, *p4;

    snprintf(path, sizeof path, "%s/HUMANS.001/APL/PANELN1.SPR", fix_dir);
    n1 = read_file(path, &n1s);
    snprintf(path, sizeof path, "%s/HUMANS.001/APL/PANELN2.SPR", fix_dir);
    n2 = read_file(path, &n2s);
    snprintf(path, sizeof path, "%s/HUMANS.001/LBM/PANEL4.BIT", fix_dir);
    p4 = read_file(path, &p4s);
    if (!n1 || !n2 || !p4 || n1s != 768u || n2s != 512u ||
        p4s < 0xFA00u + 768u) {
        fprintf(stderr, "FAIL fixture assets unreadable at %s\n", fix_dir);
        free(n1); free(n2); free(p4);
        return -1;
    }
    memcpy(strip, n1, 768u);
    memcpy(strip + 768u, n2, 512u);
    memcpy(panel4, p4, 12800u + 0xFA00u + 768u);
    free(n1); free(n2); free(p4);
    return 0;
}

/* the model: mark m's 8x8 glyph at phase ph (+ the atlas bias) */
static void model_mark(uint8_t *page, unsigned m, unsigned phase)
{
    unsigned base = 4u * (phase + MARK_BIAS(m));
    for (unsigned r = 0; r < 8u; r++)
        for (unsigned j = 0; j < 8u; j++)
            page[(MARK_Y + r) * 320u + MARK_X(m) + j] =
                (uint8_t)(0xC0u | strip[base + 8u * r + j]);
}

static void model_band(uint8_t *page)
{
    for (unsigned y = 0; y < 40u; y++)
        for (unsigned x = 0; x < 320u; x++)
            page[(BAR_Y + y) * 320u + x] = (uint8_t)(0xC0u | panel4[y * 320u + x]);
}

/* the five phases: M, S-tens (+0xC0 at stamp time), S-ones, tribe tens,
 * tribe ones */
static void model_phases(uint8_t *out, unsigned packed, unsigned diff)
{
    out[0] = (uint8_t)(((packed >> 8) + 1u) * 16u);
    out[1] = (uint8_t)((((packed >> 4) & 0xFu) + 1u) * 16u);
    out[2] = (uint8_t)(((packed & 0xFu) + 1u) * 16u);
    out[3] = (uint8_t)((diff / 10u + 1u) * 16u);
    out[4] = (uint8_t)((diff % 10u + 1u) * 16u);
}

static void model_page(uint8_t *page, const uint8_t *ph)
{
    memset(page, 0, 320u * 200u);
    model_band(page);
    for (unsigned m = 0; m < 5u; m++) model_mark(page, m, ph[m]);
}

static int diff_rows(const char *what, const uint8_t *page, const uint8_t *model)
{
    unsigned bad = 0;
    for (unsigned y = BAR_Y; y < BAR_Y + 40u; y++)
        for (unsigned x = 0; x < 320u; x++)
            if (page[y * 320u + x] != model[y * 320u + x]) {
                if (bad < 4)
                    fprintf(stderr, "FAIL %s at (%u,%u): page %02x model %02x\n",
                            what, x, y, page[y * 320u + x], model[y * 320u + x]);
                bad++;
            }
    if (bad) fprintf(stderr, "FAIL %s: %u mismatching pixels\n", what, bad);
    return bad != 0;
}

static void dump_ppm(const char *dir, const char *name, const uint8_t *page,
                     const uint8_t *pal)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (unsigned i = 0; i < 320u * 200u; i++) {
        unsigned idx = page[i];
        for (unsigned c = 0; c < 3u; c++) {
            uint8_t v6 = pal[idx * 3u + c];
            fputc((uint8_t)((v6 << 2) | (v6 >> 4)), f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;
    const char *dump_dir = argc > 3 ? argv[3] : NULL;
    char path[512];
    char fix_abs[512];
    FILE *f;
    int bad = 0;
    uint8_t model[320u * 200u];
    uint8_t ph[5];
    const uint8_t *page;

    snprintf(path, sizeof path, "%s/HUMANS.001", vol_dir);
    f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);
    if (!compat_realpath(fix_dir, fix_abs)) {
        fprintf(stderr, "FAIL fixture tree %s unreadable\n", fix_dir);
        return 1;
    }
    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }

    if (load_fixtures(fix_abs)) return 1;

    video_init(&osys_null);
    palette_init(&osys_null);
    statusbar_init(&osys_null);
    level_init(&osys_null);
    /* the level flow's disc: level_load's res_setDisc(1) — the panel
     * assets ship on HUMANS.001 (the direct statusbar_start cases below
     * run outside level_load, so install the same disc here) */
    res_setDisc(1u);

    /* 1. the seeded stamp: 6:00, the tribe surplus 12-4 = 8 -> "08" */
    statusbar_start(0x0600, 12u, 4u);
    statusbar_tick();
    page = osys_null_page(0);
    model_phases(ph, 0x0600, 12u - 4u);
    model_page(model, ph);
    bad |= diff_rows("the seeded stamp", page, model);

    /* 2. the first second rolls 6:00 -> 5:59.  On the tick that fires the
     * second the roll-start phases are stamped (the ones at 0xB0 = the
     * blank above '9'); 16 frames later the slides have settled. */
    for (unsigned i = 0; i < 69u; i++) statusbar_tick();
    statusbar_tick();                       /* tick 70: the second fires AND
                                               the slide divider (seeded 2,
                                               advancing every 2nd tick)
                                               takes its first step         */
    ph[0] = 0x6E;                          /* minutes: sliding 0x70 -> 0x60  */
    ph[1] = 0x6E;                          /* sec-tens: 0x70 -> 0x60        */
    ph[2] = 0xAE;                          /* sec-ones: 0xB0 -> 0xA0        */
    model_page(model, ph);
    bad |= diff_rows("the roll-start stamp (t=70)", page, model);
    for (unsigned i = 0; i < 16u; i++) statusbar_tick();
    ph[0] = 0x60;                          /* 5            */
    ph[1] = 0x60;                          /* 5            */
    ph[2] = 0xA0;                          /* 9            */
    model_page(model, ph);
    bad |= diff_rows("the settled 5:59 (t=86)", page, model);

    /* 3. the tribe odometer: the arm rolls the displayed pair down by one
     * (8 -> 7), sliding one glyph cell over 8 frames. */
    statusbar_armOdometer();
    statusbar_tick();                      /* the value rolls, phase stays  */
    ph[0] = 0x60; ph[1] = 0x60; ph[2] = 0xA0;
    ph[3] = 0x10;                          /* tens 0: unchanged            */
    ph[4] = 0x90;                          /* ones: still the '8' cell     */
    model_page(model, ph);
    bad |= diff_rows("the odometer roll start", page, model);
    for (unsigned i = 0; i < 8u; i++) statusbar_tick();
    ph[4] = 0x80;                          /* 7            */
    model_page(model, ph);
    bad |= diff_rows("the odometer settled (7)", page, model);

    /* 4. the time-up: a 0:00 seed dies on the first second; the marks
     * freeze and the band keeps drawing. */
    statusbar_start(0x0000, 12u, 4u);
    for (unsigned i = 0; i < 70u; i++) statusbar_tick();
    if (!statusbar_timeUp()) {
        fprintf(stderr, "FAIL the 0:00 seed did not time out\n");
        bad = 1;
    }
    ph[0] = 0x10;                          /* minutes '0' (the value died)  */
    ph[1] = 0x70;                          /* the roll-start phases frozen  */
    ph[2] = 0xB0;
    ph[3] = 0x10; ph[4] = 0x90;            /* the re-seeded pair "08"      */
    model_page(model, ph);
    bad |= diff_rows("the time-up freeze", page, model);
    for (unsigned i = 0; i < 10u; i++) statusbar_tick();
    bad |= diff_rows("the time-up still frozen", page, model);

    /* 5. the integration: level_load seeds the bar with the record's own
     * timeA and the map's 0xAA count, and level_tick composes world + bar
     * into the page. */
    if (level_load(0) != 0) {
        fprintf(stderr, "FAIL level 0 did not load\n");
        return 1;
    }
    {
        const LevelData *d = level_data();
        unsigned required = 0;
        for (unsigned i = 0; i < 640u; i++)
            if (d->map[i] == 0xAAu) required++;
        level_tick();
        model_phases(ph, d->timeA, 12u - required);
        model_page(model, ph);
        bad |= diff_rows("the level integration", page, model);
        if (statusbar_timeUp()) {
            fprintf(stderr, "FAIL the level bar timed out at once\n");
            bad = 1;
        }
        if (dump_dir) {
            /* level_fade_in completes at ~50 palette ticks (div 0x32) */
            for (unsigned i = 0; i < 60u; i++) palette_tick();
            dump_ppm(dump_dir, "statusbar_port_t0", page, osys_null_palette());
            for (unsigned i = 0; i < 90u; i++) { level_tick(); palette_tick(); }
            dump_ppm(dump_dir, "statusbar_port_t90", page, osys_null_palette());
            /* t90: one second rolled — recompute the settled phases      */
            for (unsigned i = 0; i < 16u; i++) { level_tick(); palette_tick(); }
            dump_ppm(dump_dir, "statusbar_port_t106", page, osys_null_palette());
        }
    }

    if (bad) {
        fprintf(stderr, "statusbar: FAIL\n");
        return 1;
    }
    printf("statusbar: ok\n");
    return 0;
}
