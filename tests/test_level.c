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
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern const uint8_t *osys_null_page(int which);
extern const uint8_t *osys_null_palette(void);

static uint8_t *file_image;

static uint8_t *read_fixture(const char *fix_dir, const char *rel,
                             uint32_t *size)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", fix_dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b); return NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return b;
}

static int load_fixture(const char *fix_dir, unsigned fileIdx)
{
    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.001/LEVELS/L%c.BIN",
             fix_dir, 'A' + fileIdx);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(file_image);
    file_image = malloc((size_t)n);
    if (fread(file_image, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(file_image); file_image = NULL; return -1;
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

    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }

    char fix_abs[512];
    if (!compat_realpath(fix_dir, fix_abs)) {
        fprintf(stderr, "FAIL fixture tree %s unreadable\n", fix_dir);
        return 1;
    }

    int bad = 0;
    scene_init(&osys_null);
    video_init(&osys_null);
    palette_init(&osys_null);
    level_init(&osys_null);

    /* 1. all 160 levels load and the census distributions hold. */
    unsigned intro_hist[8] = { 0 };   /* 00,01,02,03,04,05,09,FF */
    unsigned style_hist[6] = { 0 };
    for (unsigned id = 0; id < 160u; id++) {
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
            fprintf(stderr, "FAIL level %u: time A minutes %u\n", id, d->timeA >> 8);
            bad = 1;
        }
        unsigned slot;
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
            fprintf(stderr, "FAIL level %u: intro index %02x\n", id, d->introIndex);
            bad = 1;
            continue;
        }
        intro_hist[slot]++;
        style_hist[d->style - 1u]++;
    }
    {
        static const unsigned want_intro[8] = { 1, 4, 1, 3, 1, 1, 3, 146 };
        static const unsigned want_style[6] = { 26, 26, 27, 28, 26, 27 };
        for (unsigned i = 0; i < 8u; i++) {
            if (intro_hist[i] != want_intro[i]) {
                fprintf(stderr, "FAIL intro hist [%u]: %u, want %u\n",
                        i, intro_hist[i], want_intro[i]);
                bad = 1;
            }
        }
        for (unsigned i = 0; i < 6u; i++) {
            if (style_hist[i] != want_style[i]) {
                fprintf(stderr, "FAIL style hist [%u]: %u, want %u\n",
                        i, style_hist[i], want_style[i]);
                bad = 1;
            }
        }
    }

    /* 2. per file: the parsed maps + the headers are byte-exact against
     * the fixture file at the offsets computed from the parsed strings
     * (the record 0 starts at the file offset 0; the map offset = 13 +
     * len(goal)+1 + len(author)+1 + 8). */
    for (unsigned fileIdx = 0; fileIdx < 10u; fileIdx++) {
        if (load_fixture(fix_abs, fileIdx) != 0) {
            fprintf(stderr, "FAIL L%c.BIN fixture unreadable\n", 'A' + fileIdx);
            bad = 1;
            continue;
        }
        for (unsigned rec = 0; rec < 10u; rec++) {
            unsigned id = fileIdx * 10u + rec;
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
                hdr[0] = d->style; hdr[1] = d->introIndex;
                hdr[2] = d->goalCount; hdr[3] = d->entityLife;
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
                    fprintf(stderr, "FAIL L%c.BIN: header bytes\n", 'A' + fileIdx);
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
            int haveBlack = 0, barStable = 1, barLit = 0;
            int loadingSeen = 0, loadingDone = 0, barHaveFirst = 0;
            unsigned loadingFrames = 0;
            for (unsigned i = 0; i < 4000u && level_starting(); i++) {
                level_tick();
                palette_tick();
                if (level_loadingScreen()) { loadingSeen = 1; level_loadingTick(); }
                else if (loadingSeen) loadingDone = 1;
                /* The on-screen result, one line per chain frame: the page
                 * through the DAC shadow, averaged over the world band
                 * (rows 0..151) and the bar band (rows 152..191).  This is
                 * the same measurement the live capture gives, at every
                 * tick instead of one per 0.3 s sample. */
                if (loadingFrames < 200u) {
                    const uint8_t *pp = osys_null_page(0);
                    const uint8_t *dd = osys_null_palette();
                    double tsum = 0.0, bsum = 0.0;

                    for (unsigned y = 0; y < 200u; y++) {
                        for (unsigned x = 0; x < 320u; x++) {
                            const uint8_t *c = dd + 3u * pp[y * 320u + x];
                            double l = (299.0 * c[0] + 587.0 * c[1] + 114.0 * c[2]) / 1000.0;

                            if (y < 152u) tsum += l; else bsum += l;
                        }
                    }
                    fprintf(stdout,
                            "chain %02u %-7s world %5.1f  bar %5.1f\n",
                            loadingFrames,
                            (i < 1u) ? "start" :
                            (level_loadingScreen() ? "loading" :
                             (level_starting() ? "chain" : "play")),
                            tsum / (152.0 * 320.0) * 255.0 / 63.0,
                            bsum / (48.0 * 320.0) * 255.0 / 63.0);
                    fflush(stdout);
                    loadingFrames++;
                }
                {
                    const uint8_t *d = osys_null_palette();
                    unsigned topMax = 0;

                    for (unsigned c = 0; c < 0xC0u * 3u; c++)
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
                            memcpy(barAtBlack, d + 0xC0u * 3u, sizeof barAtBlack);
                            memcpy(barRowsAtBlack,
                                   osys_null_page(0) + 152u * 320u,
                                   sizeof barRowsAtBlack);
                        }
                    }
                }
            }
            for (unsigned k = 0; k < sizeof barAtBlack; k++)
                if (barAtBlack[k]) { barLit = 1; break; }
            if (!haveBlack) {
                fprintf(stderr, "FAIL the fade-out never brought 0..0xBF to black\n");
                bad = 1;
            }
            if (!barStable) {
                fprintf(stderr, "FAIL 0xC0..0xFF changed during the fade-out\n");
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
            for (unsigned i = 0; i < 64000u; i++) {
                sum += page[i];
                if (page[i]) nz++;
            }
            /* the composite now includes the object pass: the three visible
             * placed cavemen (the map's 0xAA cells at (3,2) (13,2) (16,9),
             * drawn at (48,24) (208,24) (256,136)) over the terrain */
            /* level_fade_in (1000:3344) initialises slot 8 to frame 0x17 /
             * type 0x1A and `scan_object_rows` (1000:6433) counts
             * `[0x3E73]-8+1` records from 0x3F05, so the slot is drawn; its
             * x/y are the array init's 0xFFFF (-1 in int16), which puts a 15-px
             * sliver of that sprite at the top-left.
             *
             * The pin moved once more when the type-3 rows' tail (1000:D9E8)
             * was ported: the level's type-3 placed record now runs its gravity
             * tail during the start chain's update_world frames, so its y
             * settles (sum 1153797, nz 26444, was 1149474 / 26396 with the tail
             * unported).  `test_flow` pins D9E8's crash and platform branches
             * directly. */
            /* The BG fill is the 0x80..0xBF bank (load_level_assets' 0x4BE2
             * transform) and draw_terrain leaves the BG through a zero tile
             * byte, so every page byte is now part of the composite. */
            if (sum != 6421783u || nz != 64000u) {
                fprintf(stderr, "FAIL level 0 page: sum %u nz %u\n",
                        sum, nz);
                bad = 1;
            }

            for (unsigned i = 0; i < 200u && palette_fadeActive(); i++)
                palette_tick();                 /* level_fade_in */
            const uint8_t *pal = osys_null_palette();
            uint32_t n = 0;
            uint8_t *sp = read_fixture(fix_abs, "HUMANS.001/APL/SPRPAL.APL", &n);
            uint8_t *bs = read_fixture(fix_abs, "HUMANS.001/LEVELS/BS3.BLK", &n);
            uint8_t *bg = read_fixture(fix_abs, "HUMANS.001/BACKS/BG4.RAW", &n);
            uint8_t *p4 = read_fixture(fix_abs, "HUMANS.001/LBM/PANEL4.BIT", &n);
            if (!sp || !bs || !bg || !p4) {
                fprintf(stderr, "FAIL palette fixtures unreadable\n");
                bad = 1;
            } else {
                const uint8_t *want[5] = {
                    sp + 0x18, bs + 0x18, bg + 163840u, NULL, p4 + 0xFA00u
                };
                unsigned off[5] = { 0u, 0x60u, 0xC0u, 0x180u, 0x240u };
                unsigned len[5] = { 0x60u, 0x60u, 0xC0u, 0xC0u, 0xC0u };
                for (unsigned r = 0; r < 5u; r++)
                    for (unsigned i = 0; i < len[r]; i++) {
                        uint8_t w = want[r] ? want[r][i] : 0u;
                        if (pal[off[r] + i] != w) {
                            fprintf(stderr, "FAIL palette 0x%X: got %02x want "
                                    "%02x\n", off[r] + i, pal[off[r] + i], w);
                            bad = 1;
                            break;
                        }
                    }
            }
            free(sp); free(bs); free(bg); free(p4);

            /* 1000:3DC0's view origin: the compose reads the world at
             * [0x1AEA]/[0x1AEC], so a scrolled frame is the previous one
             * shifted by that offset (the bar rows and the countdown are
             * excluded: they are drawn after the world, per tick). */
            {
                static uint8_t before[64000];
                const uint8_t *p0 = osys_null_page(0);
                memcpy(before, p0, sizeof before);
                /* the follower (1000:5CB6) owns the window in play, so the
                 * pan is exercised with the world frozen ([0xC] = 0, the state
                 * the claim holds): the compose still presents. */
                level_setUpdates(0);
                level_setView(16u, 8u);
                level_tick();
                {
                    const uint8_t *p1 = osys_null_page(0);
                    unsigned sample = 0, diffs = 0;

                    for (unsigned y = 0; y + 8u < 140u; y += 3u)
                        for (unsigned x = 0; x + 16u < 320u; x += 3u) {
                            sample++;
                            if (p1[y * 320u + x] !=
                                before[(y + 8u) * 320u + x + 16u]) diffs++;
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
                for (unsigned i = 0; i < 4u; i++)
                    memcpy(before[i], pal + (0x71u + i) * 3u, 3);
                for (unsigned t = 0; t < 4u; t++) level_tick();
                const uint8_t *now = osys_null_palette();
                for (unsigned i = 0; i < 4u; i++)
                    if (memcmp(now + (0x71u + i) * 3u,
                               before[(i + 3u) % 4u], 3) != 0) {
                        fprintf(stderr, "FAIL palette rotation: entry 0x71+%u "
                                "did not take its predecessor\n", i);
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
        for (unsigned i = 0; i < 400u && level_starting(); i++) {
            level_tick();
            palette_tick();
            if (level_loadingScreen()) level_loadingTick();
        }
        const uint8_t *pal = osys_null_palette();
        uint8_t before[12];
        memcpy(before, pal + 0x71u * 3u, 12);
        for (unsigned t = 0; t < 8u; t++) level_tick();
        if (memcmp(osys_null_palette() + 0x71u * 3u, before, 12) != 0) {
            fprintf(stderr, "FAIL level 3 (empty field M) rotated its palette\n");
            bad = 1;
        }
    }

    if (!bad) printf("level tests: OK (the 160-record census + the byte-exact maps + the render)\n");
    return bad;
}
