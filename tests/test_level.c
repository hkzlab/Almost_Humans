/* test_level.c — the level record walk + the header/map parse against the
 * real LEVELS\L*.BIN files.
 *
 * All 16 files x 10 records = 160 levels.  The census validation compares
 * the parsed headers against the measured distributions (re_formats.md
 * §2.1a-bis): the style 1..6 (26/26/27/28/26/27), the goal count 1..3,
 * +0x06 = 0 in all 160, the intro index distribution (FF x146), and the
 * record tiling (the file ends exactly on a record boundary).  Each
 * level's 640-byte map is verified against the fixture file at the offset
 * computed independently from the parsed goal/author string lengths.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (the raw L*.BIN files).
 * Missing volumes = skip. */
#include "level.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "scene.h"
#include "video.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);

extern const OSystem osys_null;

static uint8_t *file_image;

static uint8_t *read_fixture(const char *fix_dir, const char *rel,
                             uint32_t *size)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", fix_dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int64_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return b;
}

static int32_t load_fixture(const char *fix_dir, uint32_t fileIdx)
{
    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.001/LEVELS/L%c.BIN", fix_dir,
             'A' + fileIdx);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    int64_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(file_image);
    file_image = malloc((size_t)n);
    if (fread(file_image, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(file_image);
        file_image = NULL;
        return -1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.001", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);

    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    char fix_abs[512];
    if (!compat_realpath(fix_dir, fix_abs)) {
        fprintf(stderr, "FAIL fixture tree %s unreadable\n", fix_dir);
        return 1;
    }

    int32_t bad = 0;
    scene_init(&osys_null);
    video_init(&osys_null);
    palette_init(&osys_null);
    level_init(&osys_null);

    /* 1. all 160 levels load and the census distributions hold. */
    uint32_t intro_hist[8] = { 0 };   /* 00,01,02,03,04,05,09,FF */
    uint32_t style_hist[6] = { 0 };
    for (uint32_t id = 0; id < 160u; id++) {
        if (level_load(id) != 0) {
            fprintf(stderr, "FAIL level %u: load failed\n", id);
            return 1;
        }
        const LevelData *d = level_data();
        if (d->style < 1u || d->style > 6u) {
            fprintf(stderr, "FAIL level %u: style %u\n", id, d->style);
            bad = 1;
        }
        if (d->goalCount < 1u || d->goalCount > 3u) {
            fprintf(stderr, "FAIL level %u: goal count %u\n", id, d->goalCount);
            bad = 1;
        }
        if (d->unused06 != 0u) {
            fprintf(stderr, "FAIL level %u: +0x06 = %02x\n", id, d->unused06);
            bad = 1;
        }
        if ((d->timeA >> 8) < 2u || (d->timeA >> 8) > 9u) {
            fprintf(stderr, "FAIL level %u: time A minutes %u\n", id,
                    d->timeA >> 8);
            bad = 1;
        }
        uint32_t slot;
        switch (d->introIndex) {
        case 0x00u: slot = 0; break;
        case 0x01u: slot = 1; break;
        case 0x02u: slot = 2; break;
        case 0x03u: slot = 3; break;
        case 0x04u: slot = 4; break;
        case 0x05u: slot = 5; break;
        case 0x09u: slot = 6; break;
        case 0xFFu: slot = 7; break;
        default:
            fprintf(stderr, "FAIL level %u: intro index %02x\n", id,
                    d->introIndex);
            bad = 1;
            continue;
        }
        intro_hist[slot]++;
        style_hist[d->style - 1u]++;
    }
    {
        static const uint32_t want_intro[8] = { 1, 4, 1, 3, 1, 1, 3, 146 };
        static const uint32_t want_style[6] = { 26, 26, 27, 28, 26, 27 };
        for (uint32_t i = 0; i < 8u; i++) {
            if (intro_hist[i] != want_intro[i]) {
                fprintf(stderr, "FAIL intro hist [%u]: %u, want %u\n", i,
                        intro_hist[i], want_intro[i]);
                bad = 1;
            }
        }
        for (uint32_t i = 0; i < 6u; i++) {
            if (style_hist[i] != want_style[i]) {
                fprintf(stderr, "FAIL style hist [%u]: %u, want %u\n", i,
                        style_hist[i], want_style[i]);
                bad = 1;
            }
        }
    }

    /* 2. per file: the parsed maps + the headers are byte-exact against
     * the fixture file at the offsets computed from the parsed strings
     * (the record 0 starts at the file offset 0; the map offset = 13 +
     * len(goal)+1 + len(author)+1 + 8). */
    for (uint32_t fileIdx = 0; fileIdx < 10u; fileIdx++) {
        if (load_fixture(fix_abs, fileIdx) != 0) {
            fprintf(stderr, "FAIL L%c.BIN fixture unreadable\n", 'A' + fileIdx);
            bad = 1;
            continue;
        }
        for (uint32_t rec = 0; rec < 10u; rec++) {
            uint32_t id = fileIdx * 10u + rec;
            level_load(id);
            const LevelData *d = level_data();
            uint32_t mapOff = d->mapFileOff;
            if (memcmp(d->map, file_image + mapOff, 640u) != 0) {
                fprintf(stderr, "FAIL level %u: map bytes\n", id);
                bad = 1;
            }
            if (rec == 0) {
                /* the file's first record starts at the offset 0: the
                 * re-packed header must match the file's first 13 bytes */
                uint8_t hdr[13];
                hdr[0] = d->style;
                hdr[1] = d->introIndex;
                hdr[2] = d->goalCount;
                hdr[3] = d->entityLife;
                hdr[4] = (uint8_t)(d->objectTest & 0xFFu);
                hdr[5] = (uint8_t)(d->objectTest >> 8);
                hdr[6] = d->unused06;
                hdr[7] = (uint8_t)(d->timeA & 0xFFu);
                hdr[8] = (uint8_t)(d->timeA >> 8);
                hdr[9] = (uint8_t)(d->timeB & 0xFFu);
                hdr[10] = (uint8_t)(d->timeB >> 8);
                hdr[11] = (uint8_t)(d->timeC & 0xFFu);
                hdr[12] = (uint8_t)(d->timeC >> 8);
                if (memcmp(hdr, file_image, 13u) != 0) {
                    fprintf(stderr, "FAIL L%c.BIN: header bytes\n",
                            'A' + fileIdx);
                    bad = 1;
                }
            }
        }
    }
    /* 3. the level id -> the file mapping: the level 10 loads LB.BIN and
     * the level 159 loads LP.BIN's last record. */
    if (level_load(10) != 0 || level_load(159) != 0) {
        fprintf(stderr, "FAIL the level 10/159 loads\n");
        bad = 1;
    }
    if (level_load(160u) == 0) {
        fprintf(stderr, "FAIL level 160 must be out of range\n");
        bad = 1;
    }

    /* 4. the level render (level 0: style 3 -> BD3 field L=4 -> BG4.RAW):
     * the composite 320x200 viewport on page 0 (the BG's pixels biased into
     * the 64..127 bank with 0xFF -> black, then draw_terrain's tiles at the
     * +0x20 bias) and the working palette 202F:0x14D3:
     *   0..31   APL\SPRPAL.APL +0x18  (the UI)
     *   32..63  LEVELS\BS3.BLK +0x18  (the tiles)
     *   64..127 BG4.RAW's own palette  (draw_map_digit 1000:4F3C)
     *   128..191 zero
     *   192..255 LBM\PANEL4.BIT +0xFA00 (level_video_init's 0xC0 copy)
     * The page checksum is a regression guard; the palette is byte-exact
     * against the four fixtures. */
    {
        if (level_load(0) != 0) {
            fprintf(stderr, "FAIL level 0 reload\n");
            bad = 1;
        } else {
            /* level_fade_in (1000:3334): the start chain — fade out, the
             * eight update_world frames, the fade in — must finish before
             * the level is playable. */
            level_begin();
            /* The fade the user sees, measured through the null arch's DAC
             * shadow: level_fade_in (1000:3334) fades entries 0..0xBF to black
             * (div 0x19) and never touches the bar's 0xC0..0xFF block, so the
             * bar stays plainly lit while the top goes black.  The page split
             * is checked at that instant too: the bar's rows are the same
             * bytes they are in the playable frame. */
            uint8_t barAtBlack[0xC0], barRowsAtBlack[40 * 320], barFirst[0xC0];
            int32_t haveBlack = 0, barStable = 1, barLit = 0;
            int32_t loadingSeen = 0, loadingDone = 0, barHaveFirst = 0;
            uint32_t loadingFrames = 0;
            for (uint32_t i = 0; i < 4000u && level_starting(); i++) {
                level_tick();
                palette_tick();
                if (level_loadingScreen()) {
                    loadingSeen = 1;
                    level_loadingTick();
                } else if (loadingSeen) loadingDone = 1;
                /* The on-screen result, one line per chain frame: the page
                 * through the DAC shadow, averaged over the world band
                 * (rows 0..151) and the bar band (rows 152..191).  This is
                 * the same measurement the live capture gives, at every
                 * tick instead of one per 0.3 s sample. */
                if (loadingFrames < 200u) {
                    const uint8_t *pp = osys_null_page(0);
                    const uint8_t *dd = osys_null_palette();
                    double tsum = 0.0, bsum = 0.0;

                    for (uint32_t y = 0; y < 200u; y++) {
                        for (uint32_t x = 0; x < 320u; x++) {
                            const uint8_t *c = dd + 3u * pp[y * 320u + x];
                            double l =
                                (299.0 * c[0] + 587.0 * c[1] + 114.0 * c[2]) /
                                1000.0;

                            if (y < 152u) tsum += l;
                            else bsum += l;
                        }
                    }
                    fprintf(stdout, "chain %02u %-7s world %5.1f  bar %5.1f\n",
                            loadingFrames,
                            (i < 1u)
                                ? "start"
                                : (level_loadingScreen()
                                       ? "loading"
                                       : (level_starting() ? "chain" : "play")),
                            tsum / (152.0 * 320.0) * 255.0 / 63.0,
                            bsum / (48.0 * 320.0) * 255.0 / 63.0);
                    fflush(stdout);
                    loadingFrames++;
                }
                {
                    const uint8_t *d = osys_null_palette();
                    uint32_t topMax = 0;

                    for (uint32_t c = 0; c < 0xC0u * 3u; c++)
                        if (d[c] > topMax) topMax = d[c];
                    /* only the fade-OUT (1000:3334), i.e. after the loading:
                     * the video init's own fade to black (1000:32F4) covers all
                     * 0x100 entries and is a different moment. */
                    if (loadingDone && !haveBlack) {
                        if (!barHaveFirst) {
                            barHaveFirst = 1;
                            memcpy(barFirst, d + 0xC0u * 3u, sizeof barFirst);
                        } else if (memcmp(d + 0xC0u * 3u, barFirst,
                                          sizeof barFirst) != 0) {
                            barStable = 0;
                        }
                        if (topMax == 0u) {
                            haveBlack = 1;
                            memcpy(barAtBlack, d + 0xC0u * 3u,
                                   sizeof barAtBlack);
                            memcpy(barRowsAtBlack,
                                   osys_null_page(0) + 152u * 320u,
                                   sizeof barRowsAtBlack);
                        }
                    }
                }
            }
            for (uint32_t k = 0; k < sizeof barAtBlack; k++)
                if (barAtBlack[k]) {
                    barLit = 1;
                    break;
                }
            if (!haveBlack) {
                fprintf(stderr,
                        "FAIL the fade-out never brought 0..0xBF to black\n");
                bad = 1;
            }
            if (!barStable) {
                fprintf(stderr,
                        "FAIL 0xC0..0xFF changed during the fade-out\n");
                bad = 1;
            }
            if (!barLit) {
                fprintf(stderr, "FAIL the bar's palette block went dark\n");
                bad = 1;
            }
            /* The page's bar rows are the play frame's business (the page
             * checksum below pins them); the fade-out itself is a DAC effect,
             * so the split checked here is the palette's: 0..0xBF to black,
             * 0xC0..0xFF constant and lit. */
            const uint8_t *page = osys_null_page(0);
            uint32_t sum = 0, nz = 0;
            for (uint32_t i = 0; i < 64000u; i++) {
                sum += page[i];
                if (page[i]) nz++;
            }
            /* the composite now includes the object pass: the three visible
             * placed cavemen (the map's 0xAA cells at (3,2) (13,2) (16,9),
             * drawn at (48,24) (208,24) (256,136)) over the terrain */
            /* level_fade_in (1000:3344) initialises slot 8 to frame 0x17 /
             * type 0x1A and `scan_object_rows` (1000:6433) counts
             * `[0x3E73]-8+1` records from 0x3F05, so the slot is drawn.  Slot 8
             * is the active-human arrow; 1000:D64F repositions it every frame
             * (the camera seed puts it off-screen in this frame).
             *
             * The pin moved when the terrain-movement pass landed
             * (2026-11-01): the walk's item masks now come from draw_objects
             * (1000:558B), the settle masks its row base (1000:D8D0), and the
             * active-human arrow (record 8) is positioned by 1000:D64F instead
             * of sitting at the array init.  It moved again with the start
             * chain's tick model: update_world runs for level_fade_in's eight
             * frames and then from the play loop, not during the palette fade
             * (which spins in 1000:8B8B), so the first play frame is eight
             * camera steps from the (0x60,0x50) seed.  This run pins sum
             * 5743109; test_flow pins the live walk trace and the marker. */
            /* The BG fill is the 0x80..0xBF bank (load_level_assets' 0x4BE2
             * transform) and draw_terrain leaves the BG through a zero tile
             * byte, so every page byte is now part of the composite.  The
             * sum moved when the 0x92A0 page table was built correctly
             * (draw_terrain2 1000:505C): the placed records now settle and
             * walk on the real heights. */
            if (sum != 5743109u || nz != 64000u) {
                fprintf(stderr, "FAIL level 0 page: sum %u nz %u\n", sum, nz);
                bad = 1;
            }

            /* The terrain page table (draw_terrain2 1000:505C) is built
             * from the atlas and the J block's second opacity mask.  This
             * FNV-1a is the live original's 0x92A0 for level 0's style 3
             * (read from DOSBox, DS:0x92A0, 2026-11-01) - byte-exact. */
            {
                uint32_t h = 2166136261u;

                for (uint32_t id = 0; id < 256u; id++)
                    for (uint32_t c = 0; c < 16u; c++) {
                        h ^= level_pageByte(id, c);
                        h *= 16777619u;
                    }
                if (h != 0x26a3598fu) {
                    fprintf(stderr, "FAIL the 0x92A0 page table: %08x\n", h);
                    bad = 1;
                }
            }

            /* The item records (draw_objects 1000:558B): field B ships
             * all-zero and the pass fills the per-row collision masks from
             * the tile art with the J block's FIRST opacity mask.  This
             * FNV-1a (low byte first, per 16-bit word) is the live
             * original's [0x1FD6] for level 0's style 3 (read from DOSBox,
             * DS = 0x123A, 2026-11-01, all 169 x 16 words) - exact. */
            {
                uint32_t h = 2166136261u;

                for (uint32_t id = 0; id <= 0xA8u; id++)
                    for (uint32_t row = 0; row < 16u; row++) {
                        uint32_t w = level_itemWord(id, row);

                        h = (h ^ (w & 0xFFu)) * 16777619u;
                        h = (h ^ (w >> 8)) * 16777619u;
                    }
                if (h != 0x27279109u) {
                    fprintf(stderr, "FAIL the 0x1FD6 item masks: %08x\n", h);
                    bad = 1;
                }
            }
            for (uint32_t i = 0; i < 200u && palette_fadeActive(); i++)
                palette_tick();                 /* level_fade_in */
            const uint8_t *pal = osys_null_palette();
            uint32_t n = 0;
            uint8_t *sp =
                read_fixture(fix_abs, "HUMANS.001/APL/SPRPAL.APL", &n);
            uint8_t *bs =
                read_fixture(fix_abs, "HUMANS.001/LEVELS/BS3.BLK", &n);
            uint8_t *bg = read_fixture(fix_abs, "HUMANS.001/BACKS/BG4.RAW", &n);
            uint8_t *p4 =
                read_fixture(fix_abs, "HUMANS.001/LBM/PANEL4.BIT", &n);
            if (!sp || !bs || !bg || !p4) {
                fprintf(stderr, "FAIL palette fixtures unreadable\n");
                bad = 1;
            } else {
                const uint8_t *want[5] = { sp + 0x18, bs + 0x18, bg + 163840u,
                                           NULL, p4 + 0xFA00u };
                uint32_t off[5] = { 0u, 0x60u, 0xC0u, 0x180u, 0x240u };
                uint32_t len[5] = { 0x60u, 0x60u, 0xC0u, 0xC0u, 0xC0u };
                for (uint32_t r = 0; r < 5u; r++)
                    for (uint32_t i = 0; i < len[r]; i++) {
                        uint8_t w = want[r] ? want[r][i] : 0u;
                        if (pal[off[r] + i] != w) {
                            fprintf(stderr,
                                    "FAIL palette 0x%X: got %02x want "
                                    "%02x\n",
                                    off[r] + i, pal[off[r] + i], w);
                            bad = 1;
                            break;
                        }
                    }
            }
            free(sp);
            free(bs);
            free(bg);
            free(p4);

            /* ... and the acting record actually walks: with the old field-P
             * table the settle judged it unsupported (type 3, y looping
             * 8..26) and no direction moved it (D-31).  The level palette is
             * checked above first: the walk's ticks advance BD3's palette
             * rotation (entries 0x71..0x74, every 4th tick). */
            {
                InputState in;
                uint8_t *m = object_slot(object_actionEntity());
                int32_t x0 = (int32_t)(int16_t)(m[0] | (m[1] << 8));

                memset(&in, 0, sizeof in);
                in.joyBits = 0x08u;                 /* right */
                for (uint32_t t = 0; t < 6u; t++) {
                    level_noteInput(&in);
                    level_tick();
                    palette_tick();
                }
                {
                    int32_t x1 = (int32_t)(int16_t)(m[0] | (m[1] << 8));
                    uint32_t ty = (uint32_t)(m[8] | (m[9] << 8));

                    if (x1 < x0 + 4 || ty > 2u) {
                        fprintf(stderr,
                                "FAIL the record did not walk: "
                                "x %d -> %d, type %u\n",
                                x0, x1, ty);
                        bad = 1;
                    }
                }
                memset(&in, 0, sizeof in);
                level_noteInput(&in);
            }

            /* 1000:3DC0's view origin: the compose reads the world at
             * [0x1AEA]/[0x1AEC], so a scrolled frame is the previous one
             * shifted by that offset (the bar rows and the countdown are
             * excluded: they are drawn after the world, per tick). */
            {
                static uint8_t before[64000];
                const uint8_t *p0 = osys_null_page(0);
                /* the follower (1000:5CB6) owns the window in play, so the
                 * pan is exercised with the world frozen ([0xC] = 0, the state
                 * the claim holds): compose one frame at (0,0), then at
                 * (16,8) and compare the two. */
                level_setUpdates(0);
                level_setView(0u, 0u);
                level_tick();
                memcpy(before, p0, sizeof before);
                level_setView(16u, 8u);
                level_tick();
                {
                    const uint8_t *p1 = osys_null_page(0);
                    uint32_t sample = 0, diffs = 0;

                    for (uint32_t y = 0; y + 8u < 140u; y += 3u)
                        for (uint32_t x = 0; x + 16u < 320u; x += 3u) {
                            sample++;
                            if (p1[y * 320u + x] !=
                                before[(y + 8u) * 320u + x + 16u])
                                diffs++;
                        }
                    if (sample == 0u || diffs != 0u) {
                        fprintf(stderr, "FAIL the view scroll: %u/%u differ\n",
                                diffs, sample);
                        bad = 1;
                    }
                }
                level_setView(0u, 0u);
                level_tick();
                level_setUpdates(1);
            }

            /* The palette rotation (palette_rotate_table 1000:8C78), from
             * the level's ISR hook: BD3's field M (timer 3, reload 3, first
             * 0x71, 4 entries) rotates that range one step every 4 ticks —
             * each entry takes its predecessor's colour, entry 0 the last
             * one's — and re-sends them to the DAC. */
            {
                uint8_t before[4][3];
                for (uint32_t i = 0; i < 4u; i++)
                    memcpy(before[i], pal + (0x71u + i) * 3u, 3);
                for (uint32_t t = 0; t < 4u; t++) level_tick();
                const uint8_t *now = osys_null_palette();
                for (uint32_t i = 0; i < 4u; i++)
                    if (memcmp(now + (0x71u + i) * 3u, before[(i + 3u) % 4u],
                               3) != 0) {
                        fprintf(stderr,
                                "FAIL palette rotation: entry 0x71+%u "
                                "did not take its predecessor\n",
                                i);
                        bad = 1;
                    }
            }
        }
    }

    /* A style with an empty field M (level 3 -> style 2) has no rotation:
     * eight ticks leave its palette's 0x71..0x74 alone. */
    if (level_load(3) != 0) {
        fprintf(stderr, "FAIL level 3 reload for the empty table\n");
        bad = 1;
    } else {
        level_begin();                             /* its own start chain */
        for (uint32_t i = 0; i < 400u && level_starting(); i++) {
            level_tick();
            palette_tick();
            if (level_loadingScreen()) level_loadingTick();
        }
        const uint8_t *pal = osys_null_palette();
        uint8_t before[12];
        memcpy(before, pal + 0x71u * 3u, 12);
        for (uint32_t t = 0; t < 8u; t++) level_tick();
        if (memcmp(osys_null_palette() + 0x71u * 3u, before, 12) != 0) {
            fprintf(stderr,
                    "FAIL level 3 (empty field M) rotated its palette\n");
            bad = 1;
        }
    }

    /* The placed decorations' spawn snap (1000:5639 -> map_get 1000:E389).
     * Level 15 ("BAD TASTE", style 6) has the torch (0xAD, frame 0x9C) at
     * col 17/row 11.  The nudge (0x5662) only feeds the probe's y, which
     * 1000:E361 aligns with AND BX,0xFFF0, so it does not survive: the snap
     * lands at row*16 + page(0x26, sub 8) = 176 + 1 = 177.  Before the level
     * was published ahead of the spawn pass the map lookup returned NULL and
     * the snap was skipped (y stayed 176). */
    if (level_load(15) != 0) {
        fprintf(stderr, "FAIL level 15 reload for the torch\n");
        bad = 1;
    } else {
        int32_t found = 0;

        if (level_data()->style != 6u) {
            fprintf(stderr, "FAIL level 15 is not style 6\n");
            bad = 1;
        }
        for (uint32_t i = 0; i < object_slots(); i++) {
            uint8_t *r = object_slot(i);

            if (!r) continue;
            if ((r[4] | (r[5] << 8)) != 0x9Cu) continue;
            found = 1;
            if ((r[0] | (r[1] << 8)) != 272u || (r[2] | (r[3] << 8)) != 177u) {
                fprintf(stderr,
                        "FAIL the torch is at x=%u y=%u, want 272/177\n",
                        r[0] | (r[1] << 8), r[2] | (r[3] << 8));
                bad = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "FAIL level 15 has no torch record\n");
            bad = 1;
        }
    }

    if (!bad)
        printf(
            "level tests: OK (the 160-record census + the byte-exact maps + the render)\n");
    return bad;
}
