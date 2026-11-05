/* test_statusbar.c — the in-level status bar against the shipped assets.
 *
 * The bar's page composition is checked against an independent model built
 * straight from the fixture files (PANELN1.SPR + PANELN2.SPR = the glyph
 * _strip, PANEL4.BIT = the band art): the band (_panel4's top 40 rows,
 * +0xC0-tagged) and the five digit marks (the countdown's minutes /
 * sec-tens / sec-ones + the tribe pair's tens / ones, each an 8x8 glyph at
 * the (digit+1)*16 _strip slot, re_map.md → "The in-level status bar,
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
#include "level.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR_Y        152u
#define MARK_Y       172u
#define MARK_X(m)    ((const uint16_t[5]){ 8, 24, 32, 288, 296 })[m]
#define MARK_BIAS(m) ((const uint16_t[5]){ 0, 0xC0, 0, 0, 0 })[m]

/* The action row (the two icon slots at the bar's centre) is a fixed overlay
 * and is checked by _check_actions below; the band+marks model skips the
 * rectangle it covers (the slots sit at x 131..154 and 166..189). */
#define ACTION_X0 128u
#define ACTION_X1 192u

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);

extern const OSystem g_osys_null;

static uint8_t _strip[1280];
static uint8_t _panel4[12800 + 0xFA00 + 768]; /* art + the palette */
static uint8_t _icons3[4608];                /** APL\ICONS3.SPR, 12 x 0x180 */

/** @brief Read a whole file into a malloc'd buffer; NULL on error. */
static uint8_t *_read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    int64_t n;
    uint8_t *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

/** @brief Load the status bar's shipped sprite fixtures. */
static int32_t _load_fixtures(const char *fix_dir)
{
    char path[512];
    uint32_t n1s = 0, n2s = 0, p4s = 0, i3s = 0;
    uint8_t *n1, *n2, *p4, *i3;

    snprintf(path, sizeof path, "%s/HUMANS.001/APL/PANELN1.SPR", fix_dir);
    n1 = _read_file(path, &n1s);
    snprintf(path, sizeof path, "%s/HUMANS.001/APL/PANELN2.SPR", fix_dir);
    n2 = _read_file(path, &n2s);
    snprintf(path, sizeof path, "%s/HUMANS.001/LBM/PANEL4.BIT", fix_dir);
    p4 = _read_file(path, &p4s);
    snprintf(path, sizeof path, "%s/HUMANS.001/APL/ICONS3.SPR", fix_dir);
    i3 = _read_file(path, &i3s);
    if (!n1 || !n2 || !p4 || n1s != 768u || n2s != 512u ||
        p4s < 0xFA00u + 768u || !i3 || i3s != sizeof _icons3) {
        fprintf(stderr, "FAIL fixture assets unreadable at %s\n", fix_dir);
        free(n1);
        free(n2);
        free(p4);
        free(i3);
        return -1;
    }
    memcpy(_strip, n1, 768u);
    memcpy(_strip + 768u, n2, 512u);
    memcpy(_panel4, p4, 12800u + 0xFA00u + 768u);
    memcpy(_icons3, i3, sizeof _icons3);
    free(n1);
    free(n2);
    free(p4);
    free(i3);
    return 0;
}

/** the model: mark m's 8x8 glyph at phase ph (+ the atlas bias) */
static void _model_mark(uint8_t *page, uint32_t m, uint32_t phase)
{
    uint32_t base = 4u * (phase + MARK_BIAS(m));
    for (uint32_t r = 0; r < 8u; r++)
        for (uint32_t j = 0; j < 8u; j++)
            page[(MARK_Y + r) * 320u + MARK_X(m) + j] =
                (uint8_t)(0xC0u | _strip[base + 8u * r + j]);
}

/** @brief Paint the model status band from PANEL4. */
static void _model_band(uint8_t *page)
{
    for (uint32_t y = 0; y < 40u; y++)
        for (uint32_t x = 0; x < 320u; x++)
            page[(BAR_Y + y) * 320u + x] =
                (uint8_t)(0xC0u | _panel4[y * 320u + x]);
}

/** the five phases: M, S-tens (+0xC0 at stamp time), S-ones, tribe tens,
 * tribe ones */
static void _model_phases(uint8_t *out, uint32_t packed, uint32_t diff)
{
    out[0] = (uint8_t)(((packed >> 8) + 1u) * 16u);
    out[1] = (uint8_t)((((packed >> 4) & 0xFu) + 1u) * 16u);
    out[2] = (uint8_t)(((packed & 0xFu) + 1u) * 16u);
    out[3] = (uint8_t)((diff / 10u + 1u) * 16u);
    out[4] = (uint8_t)((diff % 10u + 1u) * 16u);
}

/** @brief Build the model page: blank, the band, then the five phases. */
static void _model_page(uint8_t *page, const uint8_t *ph)
{
    memset(page, 0, 320u * 200u);
    _model_band(page);
    for (uint32_t m = 0; m < 5u; m++) _model_mark(page, m, ph[m]);
}

/** @brief Compare the status band with the model and report the first diffs. */
static int32_t _diff_rows(const char *what, const uint8_t *page,
                          const uint8_t *model)
{
    uint32_t bad = 0;
    for (uint32_t y = BAR_Y; y < BAR_Y + 40u; y++)
        for (uint32_t x = 0; x < 320u; x++) {
            if (x >= ACTION_X0 && x < ACTION_X1) continue;
            if (page[y * 320u + x] != model[y * 320u + x]) {
                if (bad < 4)
                    fprintf(stderr,
                            "FAIL %s at (%u,%u): page %02x model %02x\n", what,
                            x, y, page[y * 320u + x], model[y * 320u + x]);
                bad++;
            }
        }
    if (bad) fprintf(stderr, "FAIL %s: %u mismatching pixels\n", what, bad);
    return bad != 0;
}

/** The bar's action row (blit_sprite_loop 1000:990A): [0x5B56] = 1 at the
 * level start selects 202F:0x5B2B's group 1 = ICONS3 cells 1 and 0, drawn at
 * (x+4, bar row 9) inside a 23x31 embossed slot, with the gold 0x5B57 ramp
 * frame on slot 0 only.  The slot x = (0x140-(2*23+12))/2 = 131, the pitch
 * 4+0x1F = 35. */
static int32_t _check_actions(const uint8_t *page)
{
    static const uint32_t cell[2] = { 1u, 0u };
    static const uint32_t cx[2] = { 131u + 4u, 131u + 35u + 4u };
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 2u; i++) {
        uint32_t matched = 0;
        for (uint32_t r = 0; r < 24u; r++)
            for (uint32_t c = 0; c < 16u; c++) {
                uint8_t v = _icons3[cell[i] * 0x180u + r * 16u + c];
                if (!v) continue;
                if (page[(BAR_Y + 9u + r) * 320u + cx[i] + c] ==
                    (uint8_t)(v + 0xC0u))
                    matched++;
            }
        if (matched < 200u) {
            fprintf(stderr,
                    "FAIL action slot %u holds %s (only %u of its "
                    "pixels)\n",
                    i, i ? "ICONS3 cell 0" : "cell 1", matched);
            bad = 1;
        }
    }
    /* the gold inner frame: the top edge is the ramp's byte at +6 (0xD6), the
     * bottom at +6+29 (0xD4), and the vertical edges ramp between them */
    if (page[(BAR_Y + 6u) * 320u + 132u] != 0xD6u ||
        page[(BAR_Y + 35u) * 320u + 132u] != 0xD4u) {
        fprintf(stderr,
                "FAIL the action highlight frame is missing its top/"
                "bottom edge (%02x/%02x)\n",
                page[(BAR_Y + 6u) * 320u + 132u],
                page[(BAR_Y + 35u) * 320u + 132u]);
        bad = 1;
    }
    for (uint32_t r = 0; r < 29u; r++) {          /* 0x5B57[6+r] */
        uint8_t want =
            (r <= 13u) ? (uint8_t)(0xD6u + r) : (uint8_t)(0xE3u - (r - 14u));
        if (page[(BAR_Y + 6u + r) * 320u + 132u] != want) {
            fprintf(stderr,
                    "FAIL the action highlight frame's left edge at "
                    "row %u is %02x, want %02x\n",
                    r, page[(BAR_Y + 6u + r) * 320u + 132u], want);
            bad = 1;
            break;
        }
    }
    /* slot 1 is plain: no gold in its frame */
    for (uint32_t r = 0; r < 31u; r++) {
        uint8_t v = page[(BAR_Y + 5u + r) * 320u + 131u + 35u];
        if (v >= 0xD0u && v <= 0xE3u) {
            fprintf(stderr, "FAIL slot 1 carries the gold highlight\n");
            bad = 1;
            break;
        }
    }
    return bad;
}

/** @brief Write a page and its palette as a binary PPM for inspection. */
static void _dump_ppm(const char *dir, const char *name, const uint8_t *page,
                      const uint8_t *pal)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (uint32_t i = 0; i < 320u * 200u; i++) {
        uint32_t idx = page[i];
        for (uint32_t c = 0; c < 3u; c++) {
            uint8_t v6 = pal[idx * 3u + c];
            fputc((uint8_t)((v6 << 2) | (v6 >> 4)), f);
        }
    }
    fclose(f);
}

/** @brief Run the status bar tests. */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;
    const char *dump_dir = argc > 3 ? argv[3] : NULL;
    char path[512];
    char fix_abs[512];
    FILE *f;
    int32_t bad = 0;
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
    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    if (_load_fixtures(fix_abs)) return 1;

    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    statusbar_init(&g_osys_null);
    level_init(&g_osys_null);
    /* the level flow's disc: level_load's res_set_disc(1) — the panel
     * assets ship on HUMANS.001 (the direct statusbar_start cases below
     * run outside level_load, so install the same disc here) */
    res_set_disc(1u);

    /* 1. the seeded stamp: 6:00, the tribe surplus 12-4 = 8 -> "08".  The
     * ticking gate [0x5B12] is level_fade_in's, so arm it here as the end of
     * the start chain does (level.c's level_begin). */
    statusbar_start(0x0600, 12u, 4u);
    /* 1000:3040/331E: [0x5B0A] starts at 0xC9 and decrements to 0x98. */
    {
        uint32_t i;

        if (statusbar_split() != 0xC9u) {
            fprintf(stderr, "FAIL 1000:3040: split starts at %u, want 0xC9\n",
                    statusbar_split());
            bad = 1;
        }
        for (i = 0; i < 48u; i++) statusbar_tick();
        if (statusbar_split() != 0x99u) {
            fprintf(stderr, "FAIL 1000:331E: 48 ticks leave %u, want 0x99\n",
                    statusbar_split());
            bad = 1;
        }
        statusbar_tick();
        if (statusbar_split() != 0x98u) {
            fprintf(stderr,
                    "FAIL 1000:331E: the 49th tick leaves %u, want 0x98\n",
                    statusbar_split());
            bad = 1;
        }
        statusbar_tick();
        if (statusbar_split() != 0x98u) {
            fprintf(stderr, "FAIL 1000:3328: the split moved past 0x98 (%u)\n",
                    statusbar_split());
            bad = 1;
        }
    }
    /* 1000:342B draw_level_text: [0x5B0A] = 0xC9 and crtc_set_scroll park the
     * split off-screen, so the bar is not composed over the world the end
     * page re-renders (its 0xC0 bank would otherwise show through the score
     * card's palette). */
    {
        uint8_t *pg2 = (uint8_t *)osys_null_page(0);
        uint32_t nz = 0;

        memset(pg2, 0x5A, 320u * 200u);
        statusbar_hide();
        if (statusbar_split() != 0xC9u) {
            fprintf(stderr, "FAIL 1000:342B: split %u, want 0xC9\n",
                    statusbar_split());
            bad = 1;
        }
        statusbar_tick();
        for (uint32_t i = 0; i < 320u * 200u; i++)
            if (pg2[i] != 0x5A) nz++;
        if (nz != 0u) {
            fprintf(stderr, "FAIL 1000:342B: the hidden bar wrote %u bytes\n",
                    nz);
            bad = 1;
        }
    }

    /* the band follows the split: at [0x5B0A]=160 the bar starts at row 159
     * (the tick decrements first), and row 158 is still the world. */
    {
        uint8_t *pg = (uint8_t *)osys_null_page(0);

        memset(pg, 0, 320u * 200u);
        statusbar_set_split(160u);
        statusbar_tick();
        if (pg[159u * 320u + 10u] == 0u || pg[158u * 320u + 10u] != 0u) {
            fprintf(stderr, "FAIL the band does not follow [0x5B0A]\n");
            bad = 1;
        }
    }
    statusbar_set_split(152u);
    statusbar_set_active(1);
    statusbar_tick();
    page = osys_null_page(0);
    _model_phases(ph, 0x0600, 12u - 4u);
    _model_page(model, ph);
    bad |= _diff_rows("the seeded stamp", page, model);
    bad |= _check_actions(page);
    /* 2. the first second rolls 6:00 -> 5:59.  On the tick that fires the
     * second the roll-start phases are stamped (the ones at 0xB0 = the
     * blank above '9'); 16 frames later the slides have settled. */
    for (uint32_t i = 0; i < 69u; i++) statusbar_tick();
    statusbar_tick();                       /* tick 70: the second fires AND
                                               the slide divider (seeded 2,
                                               advancing every 2nd tick)
                                               takes its first step         */
    ph[0] = 0x6E; /* minutes: sliding 0x70 -> 0x60  */
    ph[1] = 0x6E; /* sec-tens: 0x70 -> 0x60        */
    ph[2] = 0xAE; /* sec-ones: 0xB0 -> 0xA0        */
    _model_page(model, ph);
    bad |= _diff_rows("the roll-start stamp (t=70)", page, model);
    for (uint32_t i = 0; i < 16u; i++) statusbar_tick();
    ph[0] = 0x60; /* 5            */
    ph[1] = 0x60; /* 5            */
    ph[2] = 0xA0; /* 9            */
    _model_page(model, ph);
    bad |= _diff_rows("the settled 5:59 (t=86)", page, model);

    /* 3. the tribe odometer: the arm rolls the displayed pair down by one
     * (8 -> 7), sliding one glyph cell over 8 frames. */
    statusbar_arm_odometer();
    statusbar_tick(); /* the value rolls, phase stays  */
    ph[0] = 0x60;
    ph[1] = 0x60;
    ph[2] = 0xA0;
    ph[3] = 0x10; /* tens 0: unchanged            */
    ph[4] = 0x90; /* ones: still the '8' cell     */
    _model_page(model, ph);
    bad |= _diff_rows("the odometer roll start", page, model);
    for (uint32_t i = 0; i < 8u; i++) statusbar_tick();
    ph[4] = 0x80; /* 7            */
    _model_page(model, ph);
    bad |= _diff_rows("the odometer settled (7)", page, model);

    /* 4. the time-up: a 0:00 seed dies on the first second; the marks
     * freeze and the band keeps drawing. */
    statusbar_start(0x0000, 12u, 4u);
    statusbar_set_split(152u);
    statusbar_set_active(1);
    for (uint32_t i = 0; i < 70u; i++) statusbar_tick();
    if (!statusbar_time_up()) {
        fprintf(stderr, "FAIL the 0:00 seed did not time out\n");
        bad = 1;
    }
    ph[0] = 0x10; /* minutes '0' (the value died)  */
    ph[1] = 0x70; /* the roll-start phases frozen  */
    ph[2] = 0xB0;
    ph[3] = 0x10;
    ph[4] = 0x90; /* the re-seeded pair "08"      */
    _model_page(model, ph);
    bad |= _diff_rows("the time-up freeze", page, model);
    for (uint32_t i = 0; i < 10u; i++) statusbar_tick();
    bad |= _diff_rows("the time-up still frozen", page, model);

    /* 4b. the elapsed clock [0x5B0C..0E] (1000:96C6): one step per second,
     * packed minutes<<8 | tens<<4 | ones the way check_highscore repacks it
     * (1000:42CE) for the per-level best-time compare. */
    statusbar_start(0x0600, 12u, 4u);
    statusbar_set_split(152u); /* 6:00 back on the countdown */
    statusbar_set_active(1);
    if (statusbar_elapsed_word() != 0u) {
        fprintf(stderr, "FAIL the elapsed clock does not start at 0:00\n");
        bad = 1;
    }
    for (uint32_t i = 0; i < 70u * 65u; i++) statusbar_tick(); /* 1:05 */
    if (statusbar_elapsed_word() != 0x0105u) {
        fprintf(stderr, "FAIL 65 s of elapsed clock read %04x, want 0105\n",
                statusbar_elapsed_word());
        bad = 1;
    }
    for (uint32_t i = 0; i < 70u * 55u; i++) statusbar_tick(); /* 2:00 */
    if (statusbar_elapsed_word() != 0x0200u) {
        fprintf(stderr, "FAIL 120 s of elapsed clock read %04x, want 0200\n",
                statusbar_elapsed_word());
        bad = 1;
    }

    /* 5. the integration: level_load seeds the bar with the record's own
     * timeA and the map's 0xAA count, and level_tick composes world + bar
     * into the page. */
    if (level_load(0) != 0) {
        fprintf(stderr, "FAIL level 0 did not load\n");
        return 1;
    }
    {
        const LevelData *d = level_data();
        uint32_t required = 0;
        for (uint32_t i = 0; i < 640u; i++)
            if (d->map[i] == 0xAAu) required++;
        /* level_fade_in (1000:3334) first: the bar's ticking gate [0x5B12]
         * only closes when the start chain has finished, so the countdown
         * does not run during the fade. */
        level_begin();
        for (uint32_t i = 0; i < 400u && level_starting(); i++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        level_tick();
        _model_phases(ph, d->timeA, 12u - required);
        _model_page(model, ph);
        bad |= _diff_rows("the level integration", page, model);
        if (statusbar_time_up()) {
            fprintf(stderr, "FAIL the level bar timed out at once\n");
            bad = 1;
        }
        if (dump_dir) {
            /* level_fade_in completes at ~50 palette ticks (div 0x32) */
            for (uint32_t i = 0; i < 60u; i++) palette_tick();
            _dump_ppm(dump_dir, "statusbar_port_t0", page, osys_null_palette());
            for (uint32_t i = 0; i < 90u; i++) {
                level_tick();
                palette_tick();
            }
            _dump_ppm(dump_dir, "statusbar_port_t90", page,
                      osys_null_palette());
            /* t90: one second rolled — recompute the settled phases      */
            for (uint32_t i = 0; i < 16u; i++) {
                level_tick();
                palette_tick();
            }
            _dump_ppm(dump_dir, "statusbar_port_t106", page,
                      osys_null_palette());
        }
    }


    /* 1000:3933 (SPACE): the group header's second byte is the selected cell,
     * and the gold frame sits on that cell (1000:990A counts it down per
     * slot).  Step it and the highlight moves to slot 1; past the count it
     * wraps back to 1. */
    {
        const uint8_t *pg = osys_null_page(0);
        uint32_t gold0 = (BAR_Y + 6u) * 320u + 132u; /* slot 0, +1 */
        uint32_t gold1 = (BAR_Y + 6u) * 320u + 167u; /* slot 1, +1 */

        statusbar_cycle_action(); /* 1 -> 2 */
        statusbar_tick();
        if (statusbar_action_cell(1u) != 2u || pg[gold1] != 0xD6u ||
            pg[gold0] == 0xD6u) {
            fprintf(stderr,
                    "FAIL SPACE does not move the gold frame to "
                    "cell 2 (cell %u, %02x/%02x)\n",
                    statusbar_action_cell(1u), pg[gold0], pg[gold1]);
            bad = 1;
        }
        statusbar_cycle_action(); /* 2 -> wrap 1 */
        statusbar_tick();
        if (statusbar_action_cell(1u) != 1u || pg[gold0] != 0xD6u) {
            fprintf(stderr, "FAIL the action cell does not wrap to 1\n");
            bad = 1;
        }
    }

    /* D-81: the charge loop accumulates (1000:3702 draws one stroke per
     * frame; 0x36DB pre-draws +0xE = 0 and 2).  The port recomposes the page
     * each tick, so objChargeDraw redraws the whole run; the count must grow
     * with +0xE instead of showing a single stroke. */
    {
        const uint8_t *pg = osys_null_page(0);
        uint8_t *a;
        uint32_t c1 = 0, c2 = 0;

        object_init();
        a = object_slot(0);
        object_set_action_entity(0u);
        object_action_set_program(2u);
        object_action_set_cursor(2u); /* program 2, cell 2 = op 4 */
        object_set_action_latched(0);
        object_set_slot_class(1u, 5u);
        a[7] = 1u;
        a[8] = 2;
        a[9] = 0;
        object_action_run(); /* the wind-up */
        object_set_action_latched(0); /* its release */

#define BAR_SHADE()                                                            \
    ({                                                                         \
        uint32_t _n = 0;                                                       \
        for (uint32_t _y = 160u; _y < 180u; _y++)                              \
            for (uint32_t _x = 0; _x < 320u; _x++) {                           \
                uint8_t _v = pg[_y * 320u + _x];                               \
                if (_v >= 0xDFu && _v <= 0xE3u) _n++;                          \
            }                                                                  \
        _n;                                                                    \
    })
        (void)object_command_run(a, 0x80u); /* a held frame */
        object_pending_run();
        if (!object_charge_active() || object_charge_tick() != 1) {
            fprintf(stderr, "FAIL D-81: the charge loop did not start\n");
            bad = 1;
        }
        c1 = BAR_SHADE();
        for (uint32_t k = 0; k < 2u; k++) { /* +0xE -> 8, 0xC */
            (void)object_command_run(a, 0x80u);
            object_pending_run();
            (void)object_charge_tick();
        }
        c2 = BAR_SHADE();
        if (c2 <= c1 + 10u) {
            fprintf(stderr,
                    "FAIL D-81: the bar does not accumulate (%u -> %u)\n", c1,
                    c2);
            bad = 1;
        }
#undef BAR_SHADE
    }

    if (bad) {
        fprintf(stderr, "statusbar: FAIL\n");
        return 1;
    }
    printf("statusbar: ok\n");
    return 0;
}
