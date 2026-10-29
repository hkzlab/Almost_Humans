/* test_fadeladder.c — the port's own fade ladder, dumped per tick for
 * comparison against the original's recorded DAC reads (2026-10-30,
 * `OUT 3C7/IN 3C9` per entry at both phases — re_map.md, "the party's
 * entry-palette source").  Measured ORIGINAL ground truth:
 *
 *   the blues phase (end of the entry 0x100 fade — 28d0 has already run at
 *   25da, so the window's target is the replicated ramp, not the .PAL's
 *   grey upper half): window 0x81 = 00000b (.PAL[1]), 0x85 = 00000e
 *   (.PAL[5]), 0x78 = 200620 (.PAL[120]), 0xa0 = 000000 (.PAL[0]),
 *   0xc0 = 000000 (.PAL[0]), 0xe0 = 000000 (.PAL[0]),
 *   0xff = 1a0722 (.PAL[31]).
 *   The 0xff value is the discriminator: window 0x80+j takes the buffer
 *   byte 3j, and 28d0 replicates .PAL[0..31] four times over 0x4d3c, so
 *   j = 127 lands on .PAL[31] — not on .PAL[127], which is what a straight
 *   .PAL[0..127] window would give.
 *
 *   the grey phase (2b9c done): window 0x81..0x9f = 222222-flat,
 *   0xa0 = 0, 0xc0 = 0, 0xe0 = 0 (the per-32 separators), 0xff = 3a3a3a
 *   — i.e. the snapshot taken at 25bc, which is the .PAL's own upper half.
 *
 * This harness reproduces that measurement from the port's side: it drives
 * party_intro_start + party_intro_tick one tick at a time (the palette step
 * runs per tick as the ISR body does), prints the DAC triplets at milestone
 * ticks, and asserts both phases.
 *
 * argv[1] = the volume dir, argv[2] = the extracted tree.  Missing data =
 * skip with a notice (test_party's contract). */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;

static void dump_window(const uint8_t *dac, unsigned tick)
{
    printf("--- tick %u (active=%d):\n", tick, palette_fadeActive());
    printf("    0x78-0x7f: ");
    for (unsigned e = 0x78u; e <= 0x7fu; e++)
        printf("%02x%02x%02x ", dac[e*3u], dac[e*3u + 1u], dac[e*3u + 2u]);
    printf("\n    0x80-0x8f: ");
    for (unsigned e = 0x80u; e <= 0x8fu; e++)
        printf("%02x%02x%02x ", dac[e*3u], dac[e*3u + 1u], dac[e*3u + 2u]);
    printf("\n    0x90-0xff(head): 90=%02x%02x%02x a0=%02x%02x%02x ff=%02x%02x%02x\n",
           dac[0x90u*3u], dac[0x90u*3u + 1u], dac[0x90u*3u + 2u],
           dac[0xa0u*3u + 0], dac[0xa0u*3u + 1u], dac[0xa0u*3u + 2u],
           dac[0xffu*3u], dac[0xffu*3u + 1u], dac[0xffu*3u + 2u]);
}

int main(int argc, char **argv)
{
    (void)argc;
    if (argc < 3) { printf("usage: test_fadeladder <volume-dir> <tree-dir>\n"); return 1; }
    (void)argv;

    if (res_init(&osys_null) || palette_init(&osys_null) ||
        font_init(&osys_null) || video_init(&osys_null)) {
        printf("FADELADDER: init failed\n"); return 1;
    }
    res_setDisc(1);
    party_intro_setValues(1u, 4u, 12u, 0u, "DARWIN");
    if (!party_intro_start()) {
        printf("FADELADDER: SKIP (no data)\n");
        return 0;
    }

    const uint8_t *dac = palette_dacShadow();
    printf("fade: first=%u comps=%u tgt=%p workpal=%p masterpal=%p\n",
           palette_fadeFirst(), palette_fadeComps(),
           (const void *)palette_fadeTarget(),
           (const void *)party_intro_workpal(),
           (const void *)party_intro_masterpal());
    printf("fadeTarget[0..8]:  ");
    for (unsigned i = 0; i < 9u; i++) printf("%02x ", palette_fadeTarget()[i]);
    printf("\nfadeTarget[360..368]: ");
    for (unsigned i = 360u; i < 369u; i++) printf("%02x ", palette_fadeTarget()[i]);
    printf("\n");
    printf("workpal[1..7]:  ");
    for (unsigned e = 1u; e <= 7u; e++)
        printf("%02x%02x%02x ", palette_working()[e*3u], palette_working()[e*3u+1u], palette_working()[e*3u+2u]);
    printf("\nworkpal[120..127]: ");
    for (unsigned e = 120u; e <= 127u; e++)
        printf("%02x%02x%02x ", palette_working()[e*3u], palette_working()[e*3u+1u], palette_working()[e*3u+2u]);
    printf("\n");
    InputState in;
    memset(&in, 0, sizeof in);

    /* the entry: print at the fade-in's midpoint and end (the original's
     * 35 steps at ~18 Hz = the arrival read "0x81..0x8f = navies") */
    dump_window(dac, 0u);
    for (unsigned t = 1u; t <= 40u; t++) { palette_tick(); party_intro_tick(&in);
        printf("e t%03u a=%d 81=%02x%02x%02x 85=%02x%02x%02x 78=%02x%02x%02x\n",
               t, palette_fadeActive(),
               dac[0x81u*3u], dac[0x81u*3u+1u], dac[0x81u*3u+2u],
               dac[0x85u*3u], dac[0x85u*3u+1u], dac[0x85u*3u+2u],
               dac[0x78u*3u], dac[0x78u*3u+1u], dac[0x78u*3u+2u]);
    }

    /* drive into PH_SET1/2 and the wait: the ladder during each phase */
    uint8_t entry_dac[768];
    memcpy(entry_dac, dac, sizeof entry_dac);   /* the blues phase, settled */
    unsigned tick = 40u, settled_grey = 0;
    for (unsigned t = 0u; t < 900u; t++) {
        palette_tick();
        party_intro_tick(&in); tick++;
        printf("t%03u a=%d 81=%02x%02x%02x 82=%02x%02x%02x a0=%02x%02x%02x ff=%02x%02x%02x\n",
               tick, palette_fadeActive(),
               dac[0x81u*3u], dac[0x81u*3u+1u], dac[0x81u*3u+2u],
               dac[0x82u*3u], dac[0x82u*3u+1u], dac[0x82u*3u+2u],
               dac[0xa0u*3u], dac[0xa0u*3u+1u], dac[0xa0u*3u+2u],
               dac[0xffu*3u], dac[0xffu*3u+1u], dac[0xffu*3u+2u]);
        if (palette_fadeActive() && dac[0x81u*3u] == 0x22u &&
            dac[0x81u*3u+1u] == 0x22u && dac[0x81u*3u+2u] == 0x22u &&
            dac[0x8fu*3u] == 0x22u && dac[0x9fu*3u] == 0x22u &&
            dac[0xa0u*3u] == 0u && dac[0xc0u*3u] == 0u &&
            dac[0xe0u*3u] == 0u && dac[0xffu*3u] == 0x3au) settled_grey = 1;
    }
    dump_window(dac, 940u);

    /* the ground-truth asserts, one per phase (re_map.md, "the party's
     * entry-palette source": the live DAC walk of 2026-10-30) */
    int fails = 0;
    struct { unsigned e; uint8_t r, g, b; const char *why; } blues[] = {
        { 0x81u, 0x00u, 0x00u, 0x0bu, ".PAL[1]" },
        { 0x85u, 0x00u, 0x00u, 0x0eu, ".PAL[5]" },
        { 0x78u, 0x20u, 0x06u, 0x20u, ".PAL[120] (outside the 2b9c/2bda window)" },
        { 0xa0u, 0x00u, 0x00u, 0x00u, ".PAL[0] (28d0's second replication)" },
        { 0xc0u, 0x00u, 0x00u, 0x00u, ".PAL[0] (28d0's third replication)" },
        { 0xe0u, 0x00u, 0x00u, 0x00u, ".PAL[0] (28d0's fourth replication)" },
        { 0xffu, 0x1au, 0x07u, 0x22u, ".PAL[31] (the replication discriminator)" },
    };
    for (unsigned i = 0; i < sizeof blues / sizeof blues[0]; i++) {
        if (entry_dac[blues[i].e*3u] != blues[i].r || entry_dac[blues[i].e*3u+1u] != blues[i].g ||
            entry_dac[blues[i].e*3u+2u] != blues[i].b) {
            printf("FAIL: blues phase 0x%02x = %02x%02x%02x, want %s %02x%02x%02x\n",
                   blues[i].e, entry_dac[blues[i].e*3u], entry_dac[blues[i].e*3u+1u], entry_dac[blues[i].e*3u+2u],
                   blues[i].why, blues[i].r, blues[i].g, blues[i].b);
            fails++;
        }
    }
    if (!settled_grey) { printf("FAIL: grey phase (0x22 window + separators + 0x3a tail) never reached\n"); fails++; }
    if (!fails) printf("FADELADDER: all ground-truth ladder asserts pass\n");
    return fails ? 1 : 0;
}
