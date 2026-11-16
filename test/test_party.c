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
 * / DARWIN), res_set_disc(1), party_intro_set_values + start, then the
 * party_intro_tick's first compose window.  The check: the composed page vs
 * the extracted LBM/PARTY1.BIT rows byte-for-byte.
 */
#include "font.h"
#include "gameflow.h"
#include "null/osys_null.h"
#include "osystem.h"
#include "palette.h"
#include "party_intro.h"
#include "res.h"
#include "score.h"
#include "video.h"

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
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: test_party <volume-dir> <tree-dir>\n");
        return 1;
    }

    osys_bind(&g_osys_null);
    if (res_init() || palette_init() || font_init() || video_init()) {
        printf("PARTY: init failed\n");
        return 1;
    }

    /* 2a76..2a95: the marquee phase is the 0x160 word [0xf9e].  A byte-sized
     * copy wrapped at 0x100 and the bird stopped half-way across the screen. */
    {
        uint16_t src_lo = 0, dhi_lo = 0, src_hi = 0, dhi_hi = 0;

        party_intro_marquee_for_probe(0xFFu, 3u, &src_lo, &dhi_lo);
        party_intro_marquee_for_probe(0x100u, 3u, &src_hi, &dhi_hi);
        if (dhi_lo != 0x2260u + 0xFFu / 4u || dhi_hi != 0x2260u + 0x100u / 4u ||
            src_lo != (uint16_t)((3u * 4u + 3u) * 320u) ||
            src_hi != (uint16_t)((3u * 4u) * 320u)) {
            printf("PARTY: FAIL the marquee phase is not a word "
                   "(dhi %04X/%04X)\n",
                   dhi_lo, dhi_hi);
            return 1;
        }
    }

    char path[512];
    uint8_t *bit, *page, *vgapage;
    uint32_t sz = 0;

    /* the fixture: the extracted .BIT (the memset-free reference) */
    snprintf(path, sizeof path, "%s/LBM/PARTY1.BIT", argv[2]);
    bit = _read_file(path, &sz);
    if (!bit || sz < 64000u) {
        printf("PARTY: SKIP (no fixture at %s)\n", path);
        free(bit);
        return 0;
    }

    res_set_disc(1);                          // the level disc HUMANS.001

    /* level A's [live] values */
    party_intro_set_values(1u, 4u, 12u, 0u, "DARWIN");
    if (!party_intro_start()) {
        printf("PARTY: SKIP (the load path returned 0 - no volume data)\n");
        free(bit);
        return 0;
    }
    /* level_intro_screen (1000:2508) leaves the 320x192 mode-X field */
    if (osys_null_video_mode() != VIDEO_MODE_X) {
        printf("PARTY: FAIL the party did not set the mode-X field\n");
        free(bit);
        return 1;
    }

    /* one tick: the anim runs, the per-tick compose lands */
    {
        InputState in;
        memset(&in, 0, sizeof in);
        party_intro_tick(&in);
    }

    /* the second compose: the module's own exported law compose */
    page = (uint8_t *)malloc(64000u);
    party_intro_compose(page);

    vgapage = video_page(0);

    int64_t bitdiffs = 0, bankdiffs = 0;
    for (uint32_t y = 0; y < 200u; y++)
        for (uint32_t x = 0; x < 320u; x++) {
            if (page[y * 320u + x] != bit[y * 320u + x]) bitdiffs++;
            if (vgapage && vgapage[y * 320u + x] != bit[y * 320u + x])
                bankdiffs++;
        }

    if (bitdiffs == 0)
        printf(
            "PARTY: law OK - the composed page = the .BIT 320x200 verbatim\n");
    else {
        uint32_t first = (uint32_t)-1;
        for (uint32_t i = 0; i < 64000u && first == (uint32_t)-1; i++)
            if (page[i] != bit[i]) first = i;
        printf("PARTY: FAIL %" PRId64
               "/~64000 diffs, first at byte %u (%02x != %02x)\n",
               bitdiffs, first, page[first], bit[first]);
        free(bit);
        free(page);
        return 1;
    }
    if (vgapage)
        printf("PARTY: video_page(0) path diffs: %" PRId64 "\n", bankdiffs);

    /* D-18: the briefing's score is the live [0x592] low word, not 0. */
    score_set_total(0x1234u, 1u);
    party_intro_set_values(1u, 4u, 12u, gameflow_party_score(), "DARWIN");
    if (party_intro_score() != 0x1234u) {
        printf("PARTY: FAIL the briefing score is %u, want 0x1234\n",
               party_intro_score());
        free(bit);
        free(page);
        return 1;
    }
    {
        char out[8];
        party_intro_format_score(party_intro_score(), out);
        if (strcmp(out, "1234") != 0) {
            printf("PARTY: FAIL the score digits are '%s'\n", out);
            free(bit);
            free(page);
            return 1;
        }
    }

    free(bit);
    free(page);
    printf("PARTY: PASS\n");
    return 0;
}
