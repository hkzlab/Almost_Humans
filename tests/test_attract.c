/* test_attract.c - effect 4's credits scroller, byte-exact against the
 * reference dump from tools/attract_scroll_sim.py (the independent python
 * model of the strip fill + the presenter, written from the listings).
 *
 * argv[1] = the volume dir (HUMANS.00N), argv[2] = the extracted tree
 * (informational - the port loads through res), argv[3] = the reference
 * dump dir (analysis/attract_scroll: e4_strip.bin + e4_frames.bin).
 * Missing volumes or dumps = skip with a notice, like test_res.
 *
 * The run drives the attract with idle input from a fresh start into
 * effect 4 (effects 1, 3 and 2 play for real first), then checks:
 *   - the strip: all 192 KB vs e4_strip.bin, and rowsBuilt == 1579 - the
 *     fixture census (101 lines, one screenful);
 *   - the scroll: exactly 1379 ATTRACT_E4_SCROLL ticks (one strip row
 *     per display period), page 0 compared byte-for-byte at the sim's
 *     selected rows - the first draws, the skip regime, band transitions,
 *     the final rows;
 *   - the natural end: the tail fade, then the cycle continues into
 *     effect 1 with no key pressed anywhere.
 */
#include "attract.h"
#include "font.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);

extern const OSystem g_osys_null;

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
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *ref_dir = argc > 3 ? argv[3] : "../analysis/attract_scroll";

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
        int32_t bad = 0;

        for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char out[16];

            attract_format_decimal_for_probe(cases[i].v, out);
            if (strcmp(out, cases[i].s) != 0) {
                fprintf(stderr, "FAIL D-134: %u printed \"%s\", want \"%s\"\n",
                        cases[i].v, out, cases[i].s);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("ok D-134: format_decimal prints every value in order\n");
    }

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/e4_strip.bin", ref_dir);
    uint32_t strip_size = 0;
    uint8_t *ref_strip = _read_file(path, &strip_size);
    snprintf(path, sizeof path, "%s/e4_frames.bin", ref_dir);
    uint32_t frames_size = 0;
    uint8_t *ref_frames = _read_file(path, &frames_size);
    if (!ref_strip || strip_size != 196608u || !ref_frames ||
        frames_size < 4u) {
        printf(
            "skip: no reference dump at %s (run tools/attract_scroll_sim.py)\n",
            ref_dir);
        free(ref_strip);
        free(ref_frames);
        return 0;
    }
    /* the dump's frame table: u32 count, then per frame u32 row + 64000 B */
    uint32_t frame_count = 0;
    memcpy(&frame_count, ref_frames, 4);

    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    int32_t bad = 0;
    {
        const OSystem *os = &g_osys_null;
        if (res_init(os) || video_init(os) || palette_init(os) ||
            font_init(os) || attract_init(os)) {
            fprintf(stderr, "FAIL: module init\n");
            return 1;
        }
    }

    InputState idle;
    memset(&idle, 0, sizeof idle);

    /* 1. into effect 4: effects 1, 3 and 2 play for real (~12 k ticks) */
    uint32_t guard = 0;
    while (attract_phase_for_probe() != ATTRACT_E4_FADEWAIT) {
        attract_tick(&idle);
        palette_tick();
        if (++guard > 60000u) {
            fprintf(stderr,
                    "FAIL: never reached effect 4 (tick %u, phase %u)\n", guard,
                    attract_phase_for_probe());
            return 1;
        }
    }

    /* 2. the strip, byte-exact, and the census */
    {
        uint32_t rows = 0;
        const uint8_t *strip = attract_strip_for_probe(&rows);
        if (rows != 1579u) {
            fprintf(stderr,
                    "FAIL: rowsBuilt %u, want 1579 (the fixture census)\n",
                    rows);
            bad = 1;
        }
        int32_t at = _first_diff(strip, ref_strip, strip_size);
        if (at >= 0) {
            fprintf(stderr,
                    "FAIL: strip diff at byte %d (row %d): "
                    "%02x vs %02x\n",
                    at, at / 80, strip[at], ref_strip[at]);
            bad = 1;
        } else {
            printf("ok strip: 192 KB byte-identical, 1579 rows built\n");
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
            if (++total > 20000u) {
                fprintf(stderr, "FAIL: the scroll never completed\n");
                bad = 1;
                break;
            }
            if (ph != ATTRACT_E4_SCROLL) continue;   /* still fading in */
            /* this tick presented strip row `row` */
            if (cursor + 4u <= end) {
                uint32_t want;
                memcpy(&want, cursor, 4);
                if (want == row) {
                    const uint8_t *page = osys_null_page(0);
                    int32_t at = _first_diff(page, cursor + 4, 64000);
                    if (at >= 0) {
                        fprintf(stderr,
                                "FAIL: frame r%u diff at pixel %d "
                                "(y %d, x %d): %02x vs %02x\n",
                                row, at, at / 320, at % 320, page[at],
                                cursor[4 + at]);
                        bad = 1;
                    }
                    matched++;
                    cursor += 4 + 64000;
                }
            }
            row++;
        }
        if (row != 1379u) {
            fprintf(stderr, "FAIL: %u scroll rows, want 1379 (1579-0xC8)\n",
                    row);
            bad = 1;
        }
        if (matched != frame_count) {
            fprintf(stderr, "FAIL: %u/%u reference frames checked\n", matched,
                    frame_count);
            bad = 1;
        }
        if (!bad)
            printf("ok scroll: %u rows, %u frames byte-identical\n", row,
                   matched);
    }

    /* 4. the natural end: the tail fade, then the cycle into effect 1 */
    {
        guard = 0;
        while (attract_phase_for_probe() == ATTRACT_E4_OUT) {
            attract_tick(&idle);
            palette_tick();
            if (++guard > 1000u) {
                fprintf(stderr, "FAIL: the tail fade never completed\n");
                bad = 1;
                break;
            }
        }
        if (attract_phase_for_probe() != ATTRACT_E1_LOAD) {
            fprintf(stderr,
                    "FAIL: after effect 4 the cycle must re-enter "
                    "effect 1 (phase %u)\n",
                    attract_phase_for_probe());
            bad = 1;
        } else if (!bad) {
            printf("ok tail: the fade completed and the cycle continued\n");
        }
    }

    free(ref_strip);
    free(ref_frames);
    if (!bad) printf("attract e4 tests: OK (strip + scroller byte-exact)\n");
    return bad;
}
