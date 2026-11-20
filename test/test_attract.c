/* test_attract.c - effect 4's credits scroller, byte-exact against a
 * reference dump (the strip fill + the presenter, written from the
 * listings).
 *
 * argv[1] = the volume dir (HUMANS.00N), argv[2] = the extracted tree
 * (informational - the port loads through res), argv[3] = the reference
 * dump dir (e4_strip.bin + e4_frames.bin).
 * Missing volumes or dumps = skip with a notice, like test_res.
 *
 * The run drives the attract with idle input from a fresh start into
 * effect 4 (effects 1, 3 and 2 play for real first), then checks:
 *   - effect 2's wipe: one 8x16 chunk of page 1 lands on the plane model's
 *     page pixels (the blit probe, run before the cycle starts);
 *   - the strip: all 192 KB vs e4_strip.bin, and rowsBuilt == 1579 - the
 *     fixture census (101 lines, one screenful);
 *   - the scroll: exactly 1379 ATTRACT_E4_SCROLL ticks (one strip row
 *     per display period), page 0 compared byte-for-byte at the reference's
 *     selected rows - the first draws, the skip regime, band transitions,
 *     the final rows;
 *   - the natural end: the tail fade, then the cycle continues into
 *     effect 1 with no key pressed anywhere.
 */
#include "attract.h"
#include "font.h"
#include "null/osys_null.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "video.h"
#include "compat.h"
#include "test_support.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Optional frame evidence (argv[4] = a directory): the composed 320x200 page
 * and the DAC shadow, as PPM, at each attract screen. */
static const char *_dump_dir;

/**
 * @brief Write the presented page and palette as a binary PPM.
 *
 * @param name the file stem
 */
static void _dump_ppm(const char *name)
{
    if (!_dump_dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", _dump_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", VIDEO_SCREEN_W, VIDEO_SCREEN_H);
    const uint8_t *page = osys_null_page(0);
    const uint8_t *pal = osys_null_palette();
    for (uint32_t i = 0; i < VIDEO_SCREEN_BYTES; i++) {
        uint32_t v = page[i];
        uint8_t rgb[3] = { (uint8_t)(pal[v * 3u] << 2),
                           (uint8_t)(pal[v * 3u + 1u] << 2),
                           (uint8_t)(pal[v * 3u + 2u] << 2) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/**
 * @brief Return the first differing byte index, or -1 when equal.
 *
 * @param a the first buffer
 * @param b the second buffer
 * @param n the count
 * @return the first differing index, or -1 when equal
 */
static int32_t _first_diff(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return (int32_t)i;
    return -1;
}

/**
 * @brief Run the attract-frame comparison test.
 */
static void test_attract(void)
{
    const char *vol_dir = _test_argc > 1 ? _test_argv[1] : ".";
    const char *ref_dir =
        _test_argc > 3 ? _test_argv[3] : "../analysis_artifacts/attract_scroll";
    _dump_dir = _test_argc > 4 ? _test_argv[4] : NULL;

    /* D-134: 1000:0AEC builds the digits least-significant first and reverses
     * the whole buffer (0x0B3B); the low four digits are NOT exempt. */
    {
        static const struct {
            uint32_t v;
            const char *s;
        } cases[] = {
            { 0u, "0" },         { 9u, "9" },         { 9999u, "9999" },
            { 10000u, "10000" }, { 10001u, "10001" }, { 12345u, "12345" },
            { 19999u, "19999" }, { 20001u, "20001" }, { 54321u, "54321" },
            { 65535u, "65535" },
        };

        for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char out[16];

            attract_format_decimal_for_probe(cases[i].v, out);
            if (strcmp(out, cases[i].s) != 0)
                TEST_FAILF("D-134: %u printed \"%s\", want \"%s\"", cases[i].v,
                           out, cases[i].s);
        }
    }

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) TEST_SKIPF("no volume files at %s (fixture data absent)", vol_dir);
    fclose(f);
    snprintf(path, sizeof path, "%s/e4_strip.bin", ref_dir);
    uint32_t strip_size = 0;
    uint8_t *ref_strip = _read_file(path, &strip_size);
    snprintf(path, sizeof path, "%s/e4_frames.bin", ref_dir);
    uint32_t frames_size = 0;
    uint8_t *ref_frames = _read_file(path, &frames_size);
    if (!ref_strip || strip_size != 196608u || !ref_frames || frames_size < 4u)
        TEST_SKIPF("no reference dump at %s (run "
                   "tooling/attract_scroll_sim/attract_scroll_sim.py)",
                   ref_dir);
    /* the dump's frame table: u32 count, then per frame u32 row + 64000 B */
    uint32_t frame_count = 0;
    memcpy(&frame_count, ref_frames, 4);

    if (compat_chdir(vol_dir)) TEST_FAILF("cannot enter %s", vol_dir);

    {
        const OSystem *os = &g_osys_null;
        osys_bind(os);
        if (res_init() || video_init() || palette_init() || font_init() ||
            attract_init()) {
            TEST_FAILF("module init");
        }
    }

    InputState idle;
    memset(&idle, 0, sizeof idle);

    /* 0. Effect 2's wipe: an 8x16 chunk of page 1 must land on the page
     * pixels the plane model's per-pixel address gives.  Runs before the
     * attract starts; effect 1 clears the A000 window at its load. */
    {
        uint8_t *flat = (uint8_t *)osys_null_page(1);
        static const uint32_t xs[6] = { 0u, 1u, 3u, 4u, 316u, 319u };
        static const uint32_t rows[5] = { 0u, 8u, 88u, 168u, 183u };
        int32_t ebad = 0;

        for (uint32_t i = 0; i < VIDEO_PAGE_BYTES; i++)
            flat[i] = (uint8_t)(i * 7u + 3u);
        for (uint32_t xi = 0; xi < 6u; xi++)
            for (uint32_t ri = 0; ri < 5u; ri++) {
                uint32_t x = xs[xi], row = rows[ri];
                uint32_t base = attract_blit_strip8_for_probe(x, row);

                for (uint32_t r = 0; r < 16u; r++)
                    for (uint32_t j = 0; j < 8u; j++) {
                        uint32_t px = x + j, sy = row + r;
                        uint8_t want = flat[sy * VIDEO_SCREEN_W + px];
                        uint8_t got = video_vram_group(
                            (uint16_t)(base + sy * 80u + (px >> 2)))[px & 3u];

                        if (got != want) ebad++;
                    }
            }
        if (ebad) {
            TEST_FAILF("effect 2: %d wipe pixels differ from the plane "
                       "address",
                       ebad);
        }
    }

    /* 1. into effect 4: effects 1, 3 and 2 play for real (~12 k ticks) */
    uint32_t guard = 0, slide = 0, e2_hold = 0;
    while (attract_phase_for_probe() != ATTRACT_E4_FADEWAIT) {
        uint32_t before = attract_phase_for_probe();
        uint32_t ph;

        attract_tick(&idle);
        palette_tick();
        ph = attract_phase_for_probe();
        if (ph == ATTRACT_E1_HOLD && before != ATTRACT_E1_HOLD)
            _dump_ppm("attract_e1_gameint");
        if (ph == ATTRACT_E3_SLIDE) {
            if (before != ATTRACT_E3_SLIDE) slide = 0;
            /* the slide shows row `600 - slide` at the window top: 350 is
             * the promo page, 200 the title page */
            if (++slide == 251u) _dump_ppm("attract_e3_promo");
            else if (slide == 401u) _dump_ppm("attract_e3_title");
        }
        if (ph == ATTRACT_E2_HOLD && before != ATTRACT_E2_HOLD) {
            char name[64];
            if (e2_hold == 0u)
                snprintf(name, sizeof name, "attract_e2_top_ten_tribes");
            else
                snprintf(name, sizeof name, "attract_e2_times_%03u_%03u",
                         (e2_hold - 1u) * 10u + 1u, e2_hold * 10u);
            _dump_ppm(name);
            e2_hold++;
        }
        if (++guard > 60000u) {
            TEST_FAILF("never reached effect 4 (tick %u, phase %u)", guard,
                       attract_phase_for_probe());
        }
    }

    /* 2. the strip, byte-exact, and the census */
    {
        uint32_t rows = 0;
        const uint8_t *strip = attract_strip_for_probe(&rows);
        if (rows != 1579u)
            TEST_FAILF("rowsBuilt %u, want 1579 (the fixture census)", rows);
        int32_t at = _first_diff(strip, ref_strip, strip_size);
        if (at >= 0) {
            TEST_FAILF("strip diff at byte %d (row %d): %02x vs %02x", at,
                       at / 80, strip[at], ref_strip[at]);
        }
    }

    /* 3. the scroll: one strip row per SCROLL tick, page 0 vs the dump.
     * The fade-in (div 0x1E, ~30 ticks) still runs when we get here - the
     * FADEWAIT ticks are skipped without counting a row. */
    {
        const uint8_t *cursor = ref_frames + 4;
        const uint8_t *end = ref_frames + frames_size;
        uint32_t matched = 0, row = 0, total = 0;
        while (1) {
            uint32_t ph = attract_phase_for_probe();
            if (ph != ATTRACT_E4_SCROLL && ph != ATTRACT_E4_FADEWAIT) break;
            attract_tick(&idle);
            palette_tick();
            if (++total > 20000u) TEST_FAILF("the scroll never completed");
            if (ph != ATTRACT_E4_SCROLL) continue;   // still fading in
            /* this tick presented strip row `row` */
            if (row == 0u || row == 300u || row == 688u || row == 1378u) {
                char name[64];
                snprintf(name, sizeof name, "attract_e4_r%u", row);
                _dump_ppm(name);
            }
            if (cursor + 4u <= end) {
                uint32_t want;
                memcpy(&want, cursor, 4);
                if (want == row) {
                    const uint8_t *page = osys_null_page(0);
                    int32_t at = _first_diff(page, cursor + 4, 64000);
                    if (at >= 0) {
                        TEST_FAILF("frame r%u diff at pixel %d (y %d, x "
                                   "%d): %02x vs %02x",
                                   row, at, at / 320, at % 320, page[at],
                                   cursor[4 + at]);
                    }
                    matched++;
                    cursor += 4 + 64000;
                }
            }
            row++;
        }
        if (row != 1379u)
            TEST_FAILF("%u scroll rows, want 1379 (1579-0xC8)", row);
        if (matched != frame_count)
            TEST_FAILF("%u/%u reference frames checked", matched, frame_count);
    }

    /* 4. the natural end: the tail fade, then the cycle into effect 1 */
    {
        guard = 0;
        while (attract_phase_for_probe() == ATTRACT_E4_OUT) {
            attract_tick(&idle);
            palette_tick();
            if (++guard > 1000u) {
                TEST_FAILF("the tail fade never completed");
            }
        }
        if (attract_phase_for_probe() != ATTRACT_E1_LOAD) {
            TEST_FAILF("after effect 4 the cycle must re-enter "
                       "effect 1 (phase %u)",
                       attract_phase_for_probe());
        }
    }

    free(ref_strip);
    free(ref_frames);
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_attract);
    return UNITY_END();
}
