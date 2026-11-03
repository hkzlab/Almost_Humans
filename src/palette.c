/* palette.c — fades and DAC (game_structure.md §1 subsystem 4, §6).
 *
 * The original's machine, ported 1:1: a 256-entry 6-bit working palette
 * (202F:0x4BBC) refilled by palette_load_6bit (1000:8d24) from each loaded
 * image — the RAM copy only, it never touches the DAC — and the DAC fade
 * state (ramp 202F:0x4EBC, control 0x48B0..0x48B4) stepped once per tick by
 * palette_fade_step (1000:8bfd) from the ISR body.  palette_build_fade
 * (1000:8bac) reads the CURRENT DAC into the ramp and stores per-component
 * deltas (target-current)<<8/div; each tick adds one delta, clamps at the
 * target and clears the active flag once every component reached it. */
#include "game.h"

static const OSystem *pal_osys;
static uint8_t working[768];   /* 202F:0x4BBC — the working palette       */
static uint8_t dac[768];       /* the DAC's 6-bit shadow (8bac's IN 3C9h) */

/* the fade state: 202F:0x48B0/48B2/48B6/48B8 + the 0x4EBC ramp          */
static uint32_t fadeFirst;    /* [0x48B8]: the DAC's first entry    */
static uint32_t fadeComps;    /* [0x48B0]: components (entries * 3) */
static const uint8_t *fadeTarget;   /* [0x48B6]: the target palette bytes */
static int32_t fadeActive;   /* [0x48B4]                           */
static uint16_t ramp[768][2]; /* [0x4EBC]: {cur<<8, delta<<8}       */

int32_t palette_init(const OSystem *osys)
{
    pal_osys = osys;
    return 0;
}

/* palette_load_6bit (1000:8d24): the working palette := 768 6-bit bytes. */
void palette_loadWorking(const uint8_t *p6)
{
    for (uint32_t i = 0; i < 768u; i++) working[i] = p6[i] & 0x3Fu;
}

const uint8_t *palette_working(void) { return working; }

/* seal_triples (1000:43C2) + hash_three_bytes (1000:43D5): turn `entries`
 * triples of the given palette buffer into grey.  The hash is
 * (77R + 151G + 28B) >> 8 and the tail's SHR CH,1 halves it, so the grey
 * value is (77R + 151G + 28B) >> 9 (0..31 on 6-bit components); all three
 * components become it.  draw_level_text (0x33E2) seals the level palette's
 * first 0x80 entries; the attract's decode_page_in_place (0x15F5) uses the
 * same value per page byte. */
void palette_sealTriples(uint8_t *p6, uint32_t first, uint32_t entries)
{
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t at = (first + i) * 3u;
        uint32_t r = p6[at], g = p6[at + 1u], b = p6[at + 2u];
        uint32_t v = (77u * r + 151u * g + 28u * b) >> 9;

        p6[at] = p6[at + 1u] = p6[at + 2u] = (uint8_t)v;
    }
}

/* set_palette_dac (1000:8CEE) route: an immediate DAC write that keeps the
 * shadow honest for the next fade's current-colour read. */
void palette_setDac(const uint8_t *p6, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n * 3u; i++) dac[first * 3u + i] = p6[i] & 0x3Fu;
    pal_osys->videoSetPalette(p6, first, n);
}

/* palette_build_fade (1000:8bac): fade DAC entries first..first+entries-1
 * toward `target` (the 6-bit palette whose triple for entry `first` is
 * target[0]) in ~div ticks. */
void palette_buildFade(uint32_t first, uint32_t entries, uint32_t div,
                       const uint8_t *target)
{
    fadeFirst = first;
    fadeComps = entries * 3u;
    fadeTarget = target;
    for (uint32_t c = 0; c < fadeComps; c++) {
        uint32_t cur = dac[first * 3u + c];
        uint32_t tgt = target[c];
        ramp[c][0] = (uint16_t)(cur << 8);
        ramp[c][1] =
            (uint16_t)((((int32_t)tgt - (int32_t)cur) * 256) / (int32_t)div);
    }
    fadeActive = 1;
}

/* palette_fade_active (1000:8b9a) */
int32_t palette_fadeActive(void) { return fadeActive; }

/* the DAC shadow, read-only: the fade-ladder harness compares the port's
 * per-tick DAC progression against the original's live IN 3C9h reads
 * (2026-10-30: window 0x80+n <- buffer entry n, verified in the hardware).
 * No engine logic lives here. */
const uint8_t *palette_dacShadow(void) { return dac; }
const uint8_t *palette_fadeTarget(void) { return fadeTarget; }
uint32_t palette_fadeFirst(void) { return fadeFirst; }
uint32_t palette_fadeComps(void) { return fadeComps; }

/* palette_fade_step (1000:8bfd): one step per tick (the ISR body's palette
 * work, game_main's game_tick). */
void palette_tick(void)
{
    if (!fadeActive) return;
    uint32_t done = 0;
    for (uint32_t c = 0; c < fadeComps; c++) {
        uint16_t cur = ramp[c][0];
        uint8_t t = fadeTarget[c];
        uint8_t v = (uint8_t)(cur >> 8);
        if (v != t) {
            cur = (uint16_t)(cur + ramp[c][1]);
            ramp[c][0] = cur;
            v = (uint8_t)(cur >> 8);
            if (ramp[c][1] & 0x8000u) {           /* negative delta        */
                if (v < t) {
                    v = t;
                    ramp[c][0] = (uint16_t)(((uint32_t)t << 8) | (cur & 0xffu));
                }
            } else {
                if (v > t) {
                    v = t;
                    ramp[c][0] = (uint16_t)(((uint32_t)t << 8) | (cur & 0xffu));
                }
            }
        } else {
            done++;
        }
        dac[fadeFirst * 3u + c] = v;
    }
    if (done == fadeComps) fadeActive = 0;
    pal_osys->videoSetPalette(dac + fadeFirst * 3u, fadeFirst, fadeComps / 3u);
}
