/* test_party.c — the level party screen's bake-frame check, byte-exact
 * against the closed A000↔.BIT↔screen layout law (2026-10-29 session):
 *
 * the composed page's pixel (x,y) = the .BIT's pixel (x,y), 320x200, 1:1 per
 * row (the bake's pitch 88 with the +8 head; the band rows 20..87 = the
 * canvas rows; the law's sy = 20 PNG law peak agrees).
 *
 * argv[1] = the volume dir (HUMANS.00N), argv[2] = the extracted tree.
 * Missing data = skip with a notice (test_res's contract).
 *
 * The drive: the values are level A's ([live] need 4 / members 12 / score 0
 * / DARWIN), res_setDisc(1), party_intro_setValues + start, then the
 * party_intro_tick's first compose window.  The check: the composed page vs
 * the extracted LBM/PARTY1.BIT rows byte-for-byte.
 */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern VideoMode osys_null_videoMode(void);

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_party <volume-dir> <tree-dir>\n"); return 1; }

    if (res_init(&osys_null) || palette_init(&osys_null) ||
        font_init(&osys_null) || video_init(&osys_null)) {
        printf("PARTY: init failed\n"); return 1;
    }

    char path[512];
    uint8_t *bit, *page, *vgapage;
    uint32_t sz = 0;

    /* the fixture: the extracted .BIT (the memset-free reference) */
    snprintf(path, sizeof path, "%s/LBM/PARTY1.BIT", argv[2]);
    bit = read_file(path, &sz);
    if (!bit || sz < 64000u) {
        printf("PARTY: SKIP (no fixture at %s)\n", path);
        free(bit); return 0;
    }

    res_setDisc(1);                          /* the level disc HUMANS.001 */

    /* level A's [live] values */
    party_intro_setValues(1u, 4u, 12u, 0u, "DARWIN");
    if (!party_intro_start()) {
        printf("PARTY: SKIP (the load path returned 0 — no volume data)\n");
        free(bit); return 0;
    }
    /* level_intro_screen (1000:2508) leaves the 320x192 mode-X field */
    if (osys_null_videoMode() != VIDEO_MODE_X) {
        printf("PARTY: FAIL the party did not set the mode-X field\n");
        free(bit); return 1;
    }

    /* one tick: the anim runs, the per-tick compose lands */
    { InputState in; memset(&in, 0, sizeof in); party_intro_tick(&in); }

    /* the second compose: the module's own exported law compose */
    page = (uint8_t *)malloc(64000u);
    party_intro_compose(page);

    vgapage = video_page(0);

    long bitdiffs = 0, bankdiffs = 0;
    for (unsigned y = 0; y < 200u; y++)
        for (unsigned x = 0; x < 320u; x++) {
            if (page[y * 320u + x] != bit[y * 320u + x]) bitdiffs++;
            if (vgapage && vgapage[y * 320u + x] != bit[y * 320u + x])
                bankdiffs++;
        }

    if (bitdiffs == 0)
        printf("PARTY: law OK — the composed page = the .BIT 320x200 verbatim\n");
    else {
        unsigned first = (unsigned)-1;
        for (unsigned i = 0; i < 64000u && first == (unsigned)-1; i++)
            if (page[i] != bit[i]) first = i;
        printf("PARTY: FAIL %ld/~64000 diffs, first at byte %u (%02x != %02x)\n",
               bitdiffs, first, page[first], bit[first]);
        free(bit); free(page); return 1;
    }
    if (vgapage)
        printf("PARTY: video_page(0) path diffs: %ld\n", bankdiffs);

    /* D-18: the briefing's score is the live [0x592] low word, not 0. */
    score_setTotal(0x1234u, 1u);
    party_intro_setValues(1u, 4u, 12u, gameflow_partyScore(), "DARWIN");
    if (party_intro_score() != 0x1234u) {
        printf("PARTY: FAIL the briefing score is %u, want 0x1234\n",
               party_intro_score());
        free(bit); free(page);
        return 1;
    }
    {
        char out[8];
        party_intro_formatScore(party_intro_score(), out);
        if (strcmp(out, "1234") != 0) {
            printf("PARTY: FAIL the score digits are '%s'\n", out);
            free(bit); free(page);
            return 1;
        }
    }

    free(bit); free(page);
    printf("PARTY: PASS\n");
    return 0;
}
