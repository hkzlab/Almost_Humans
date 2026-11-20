/* test_party.c - the level party screen's bake-frame check, byte-exact
 * against the closed A000<->.BIT<->screen layout law (2026-10-29 session):
 *
 * the composed page's pixel (x,y) = the .BIT's pixel (x,y), 320x200, 1:1 per
 * row (the bake's pitch 88 with the +8 head; the band rows 20..87 = the
 * canvas rows; the law's sy = 20 PNG law peak agrees).
 *
 * argv[1] = the volume dir (HUMANS.00N), argv[2] = the extracted tree.
 * Missing data = skip with a notice (test_res's contract).
 *
 * The drive: the values are level A's ([live] need 4 / members 12 / score 0
 * / DARWIN), res_set_disc(1), party_intro_set_values + start, the blocking
 * audio fade-in driven to its end, then one tick.  The check: a clean bake
 * of both pages (the probe) vs the extracted LBM/PARTY1.BIT rows
 * byte-for-byte, on the exported compose and on video_page(0).
 */
#include "audio.h"
#include "compat.h"
#include "font.h"
#include "gameflow.h"
#include "null/osys_null.h"
#include "osystem.h"
#include "palette.h"
#include "party_intro.h"
#include "res.h"
#include "score.h"
#include "video.h"
#include "test_support.h"

#include <inttypes.h>
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
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
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
 * @brief Run the party briefing tests.
 */
static void test_party(void)
{
    if (_test_argc < 3) TEST_FAILF("usage: test_party <volume-dir> <tree-dir>");

    osys_bind(&g_osys_null);
    if (res_init() || palette_init() || font_init() || video_init())
        TEST_FAILF("PARTY: init failed");

    /* 2a76..2a95: the marquee phase is the 0x160 word [0xf9e].  A byte-sized
     * copy wrapped at 0x100 and the bird stopped half-way across the screen. */
    {
        uint16_t src_lo = 0, dhi_lo = 0, src_hi = 0, dhi_hi = 0;

        party_intro_marquee_for_probe(0xFFu, 3u, &src_lo, &dhi_lo);
        party_intro_marquee_for_probe(0x100u, 3u, &src_hi, &dhi_hi);
        if (dhi_lo != 0x2260u + 0xFFu / 4u || dhi_hi != 0x2260u + 0x100u / 4u ||
            src_lo != (uint16_t)((3u * 4u + 3u) * 320u) ||
            src_hi != (uint16_t)((3u * 4u) * 320u))
            TEST_FAILF("the marquee phase is not a word "
                       "(dhi %04X/%04X)",
                       dhi_lo, dhi_hi);
    }

    char path[512];
    uint8_t *bit, *page, *vgapage;
    uint32_t sz = 0;

    /* the fixture: the extracted .BIT (the memset-free reference).  The tree
     * is either flat (LBM/) or the per-disc extraction (HUMANS.001/LBM/). */
    snprintf(path, sizeof path, "%s/LBM/PARTY1.BIT", _test_argv[2]);
    bit = _read_file(path, &sz);
    if (!bit) {
        snprintf(path, sizeof path, "%s/HUMANS.001/LBM/PARTY1.BIT",
                 _test_argv[2]);
        bit = _read_file(path, &sz);
    }
    if (!bit || sz < 64000u) {
        free(bit);
        TEST_SKIPF("(no fixture at %s)", path);
    }

    /* res opens HUMANS.00N from the working directory */
    if (compat_chdir(_test_argv[1]))
        TEST_FAILF("cannot enter %s", _test_argv[1]);

    res_set_disc(1);                          // the level disc HUMANS.001

    /* level A's [live] values */
    party_intro_set_values(1u, 4u, 12u, 0u, "DARWIN");
    if (!party_intro_start()) {
        free(bit);
        TEST_SKIPF("(the load path returned 0 - no volume data)");
    }
    /* level_intro_screen (1000:2508) leaves the 320x192 mode-X field */
    if (osys_null_video_mode() != VIDEO_MODE_X)
        TEST_FAILF("the party did not set the mode-X field");

    /* The bake waits for the blocking fade-in (1000:2564 timing_loop); the
     * port's fade is per audio tick, so advance it before the first tick. */
    {
        uint32_t guard = 0;

        while (audio_fade_busy() && ++guard < 1000u) audio_tick();
        if (audio_fade_busy()) TEST_FAILF("the audio fade-in never ended");
    }

    /* one tick: the bake + anim run, the per-tick compose lands */
    {
        InputState in;
        memset(&in, 0, sizeof in);
        party_intro_tick(&in);
    }

    /* the bake law without the first animation frame: both pages from the
     * .BIT (the tick above already exercised the real PH_MUSIC bake). */
    party_intro_bake_for_probe();

    /* the second compose: the module's own exported law compose */
    page = (uint8_t *)malloc(64000u);
    party_intro_compose(page);

    vgapage = video_page(0);
    if (vgapage) party_intro_compose(vgapage); // the backend-page path too

    int64_t bitdiffs = 0, bankdiffs = 0;
    for (uint32_t y = 0; y < 200u; y++)
        for (uint32_t x = 0; x < 320u; x++) {
            if (page[y * 320u + x] != bit[y * 320u + x]) bitdiffs++;
            if (vgapage && vgapage[y * 320u + x] != bit[y * 320u + x])
                bankdiffs++;
        }

    if (bitdiffs != 0) {
        uint32_t first = (uint32_t)-1;
        for (uint32_t i = 0; i < 64000u && first == (uint32_t)-1; i++)
            if (page[i] != bit[i]) first = i;
        TEST_FAILF("%" PRId64 "/~64000 diffs, first at byte %u (%02x != %02x)",
                   bitdiffs, first, page[first], bit[first]);
    }
    if (vgapage && bankdiffs != 0)
        TEST_FAILF("the video_page(0) path differs in %" PRId64 " pixels",
                   bankdiffs);

    /* D-18: the briefing's score is the live [0x592] low word, not 0. */
    score_set_total(0x1234u, 1u);
    party_intro_set_values(1u, 4u, 12u, gameflow_party_score(), "DARWIN");
    if (party_intro_score() != 0x1234u)
        TEST_FAILF("the briefing score is %u, want 0x1234",
                   party_intro_score());
    {
        char out[8];
        party_intro_format_score(party_intro_score(), out);
        /* the formatter is 2B70's decimal one: 0x1234 prints 4660 */
        if (strcmp(out, "4660") != 0)
            TEST_FAILF("the score digits are '%s', want \"4660\"", out);
    }

    free(bit);
    free(page);
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_party);
    return UNITY_END();
}
