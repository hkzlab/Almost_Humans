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
#include "res.h"
#include "palette.h"
#include "scene.h"
#include "video.h"
#include "compat.h"
#include "byteorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);

extern const OSystem g_osys_null;

static uint8_t *_file_image;

/**
 * @brief Read one fixture-relative file; NULL on error.
 *
 * @param fix_dir the fixture directory
 * @param rel the release flag
 * @param size the size
 * @return the pointer, or NULL when unavailable
 */
static uint8_t *_read_fixture(const char *fix_dir, const char *rel,
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

/**
 * @brief Load one shipped LEVELS/L?.BIN fixture file.
 *
 * @param fix_dir the fixture directory
 * @param fileIdx the fixture file index
 * @return 0 on success, -1 when unreadable
 */
static int32_t _load_fixture(const char *fix_dir, uint32_t fileIdx)
{
    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.001/LEVELS/L%c.BIN", fix_dir,
             'A' + fileIdx);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    int64_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(_file_image);
    _file_image = malloc((size_t)n);
    if (fread(_file_image, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(_file_image);
        _file_image = NULL;
        return -1;
    }
    fclose(f);
    return 0;
}

/**
 * @brief Run the level load and play tests.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
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
    res_init(&g_osys_null);
    scene_init(&g_osys_null);
    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    level_init(&g_osys_null);

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
        if (_load_fixture(fix_abs, fileIdx) != 0) {
            fprintf(stderr, "FAIL L%c.BIN fixture unreadable\n", 'A' + fileIdx);
            bad = 1;
            continue;
        }
        for (uint32_t rec = 0; rec < 10u; rec++) {
            uint32_t id = fileIdx * 10u + rec;
            level_load(id);
            const LevelData *d = level_data();
            uint32_t mapOff = d->mapFileOff;
            if (memcmp(d->map, _file_image + mapOff, 640u) != 0) {
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
                if (memcmp(hdr, _file_image, 13u) != 0) {
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
     *   0       the loading screen's BACKS\DICON.APL +0x18 triple (D-142)
     *   1..31  APL\SPRPAL.APL +0x1B  (the UI)
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
            /* D-257: level_video_init's reset_display_mode (1000:32D6)
             * clears the whole A000 window (clear_vram 1000:833A) BEFORE
             * the 32F4/3309 fades, so a stale pre-level page must not
             * survive level_begin. */
            memset((uint8_t *)osys_null_page(0), 0xAA, 320u * 200u);
            memset((uint8_t *)osys_null_page(1), 0xAA, 320u * 200u);
            /* level_fade_in (1000:3334): the start chain — fade out, the
             * eight update_world frames, the fade in — must finish before
             * the level is playable. */
            level_begin();
            {
                const uint8_t *p0 = osys_null_page(0);
                const uint8_t *p1 = osys_null_page(1);
                uint32_t nz0 = 0u, nz1 = 0u, k;

                for (k = 0; k < 320u * 200u; k++) {
                    if (p0[k]) nz0++;
                    if (p1[k]) nz1++;
                }
                if (nz0 || nz1) {
                    fprintf(stderr,
                            "FAIL D-257: level_begin left the page(s) "
                            "uncleared (%u/%u stale bytes)\n",
                            nz0, nz1);
                    bad = 1;
                }
            }
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
                if (level_loading_screen()) {
                    loadingSeen = 1;
                    level_loading_tick();
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
                                : (level_loading_screen()
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
            /* The three placed humans now settle through `collision_scan`
             * (1000:D860, D-89): the tile walk leaves them at row*16-7 and
             * their type-2 row hook (0xFF) runs the settle every frame, which
             * lands them one pixel higher at row*16-8 (24/136/264).  This run
             * pins sum 5742629. */
            if (sum != 5742629u || nz != 64000u) {
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
                        h ^= level_page_byte(id, c);
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
                        uint32_t w = level_item_word(id, row);

                        h = (h ^ (w & 0xFFu)) * 16777619u;
                        h = (h ^ (w >> 8)) * 16777619u;
                    }
                if (h != 0x27279109u) {
                    fprintf(stderr, "FAIL the 0x1FD6 item masks: %08x\n", h);
                    bad = 1;
                }
            }
            for (uint32_t i = 0; i < 200u && palette_fade_active(); i++)
                palette_tick();                 /* level_fade_in */
            const uint8_t *pal = osys_null_palette();
            uint32_t n = 0;
            uint8_t *sp =
                _read_fixture(fix_abs, "HUMANS.001/APL/SPRPAL.APL", &n);
            uint8_t *bs =
                _read_fixture(fix_abs, "HUMANS.001/LEVELS/BS3.BLK", &n);
            uint8_t *bg =
                _read_fixture(fix_abs, "HUMANS.001/BACKS/BG4.RAW", &n);
            uint8_t *p4 =
                _read_fixture(fix_abs, "HUMANS.001/LBM/PANEL4.BIT", &n);
            uint8_t *di =
                _read_fixture(fix_abs, "HUMANS.001/BACKS/DICON.APL", &n);
            if (!sp || !bs || !bg || !p4 || !di) {
                fprintf(stderr, "FAIL palette fixtures unreadable\n");
                bad = 1;
            } else {
                const uint8_t *want[6] = {
                    di + 0x18,    sp + 0x1B, bs + 0x18,
                    bg + 163840u, NULL,      p4 + 0xFA00u
                };
                uint32_t off[6] = { 0u, 3u, 0x60u, 0xC0u, 0x180u, 0x240u };
                uint32_t len[6] = { 3u, 0x5Du, 0x60u, 0xC0u, 0xC0u, 0xC0u };
                for (uint32_t r = 0; r < 6u; r++)
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
            free(di);

            /* ... and the acting record actually walks: with the old field-P
             * table the settle judged it unsupported (type 3, y looping
             * 8..26) and no direction moved it (D-31).  The level palette is
             * checked above first: the walk's ticks advance BD3's palette
             * rotation (entries 0x71..0x74, every 4th tick). */
            {
                InputState in;
                uint8_t *m = object_slot(object_action_entity());
                int32_t x0 = (int32_t)(int16_t)(rd_le_u16(m + 0));

                memset(&in, 0, sizeof in);
                in.joyBits = 0x08u;                 /* right */
                for (uint32_t t = 0; t < 6u; t++) {
                    level_note_input(&in);
                    level_tick();
                    palette_tick();
                }
                {
                    int32_t x1 = (int32_t)(int16_t)(rd_le_u16(m + 0));
                    uint32_t ty = (uint32_t)(rd_le_u16(m + 8));

                    if (x1 < x0 + 4 || ty > 2u) {
                        fprintf(stderr,
                                "FAIL the record did not walk: "
                                "x %d -> %d, type %u\n",
                                x0, x1, ty);
                        bad = 1;
                    }
                }
                memset(&in, 0, sizeof in);
                level_note_input(&in);
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
                level_set_updates(0);
                level_set_view(0u, 0u);
                level_tick();
                memcpy(before, p0, sizeof before);
                level_set_view(16u, 8u);
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
                level_set_view(0u, 0u);
                level_tick();
                level_set_updates(1);
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
            if (level_loading_screen()) level_loading_tick();
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

    /* D-163: the claim sequence never calls update_world, even after
     * 1000:3CAB sets [0xC] = 0xFF.  level_tick must keep the animator/bg
     * tails out until the sequence is off. */
    {
        uint8_t *a, *n;
        uint32_t ticks;

        object_init();
        object_set_player(0);
        object_set_action_entity(0u);
        a = object_slot(0);
        n = object_slot(3);
        a[0] = 0x00;
        a[1] = 0x01;
        a[2] = 0x80;
        a[3] = 0x00;
        a[6] = 0x00;
        a[8] = 0x02;
        n[0] = 0x20;
        n[1] = 0x01;
        n[2] = 0x88;
        n[3] = 0x00;
        n[6] = 0x00;
        n[8] = 0x02;
        if (object_claim_begin(0u, 0) != 3) {
            fprintf(stderr, "FAIL the D-163 claim did not start\n");
            bad = 1;
        } else {
            level_set_updates(1); /* 1000:3CAB's [0xC] = 0xFF */
            ticks = object_world_ticks_probe();
            for (uint32_t t = 0; t < 4u; t++) level_tick();
            if (object_world_ticks_probe() != ticks) {
                fprintf(stderr,
                        "FAIL update_world ran during a claim (D-163)\n");
                bad = 1;
            }
            for (uint32_t t = 0; t < 20000u && object_claim_active(); t++) {
                level_tick();
                palette_tick();
            }
        }
        object_init();
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
            if ((rd_le_u16(r + 4)) != 0x9Cu) continue;
            found = 1;
            if ((rd_le_u16(r + 0)) != 272u || (rd_le_u16(r + 2)) != 177u) {
                fprintf(stderr,
                        "FAIL the torch is at x=%u y=%u, want 272/177\n",
                        rd_le_u16(r + 0), rd_le_u16(r + 2));
                bad = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "FAIL level 15 has no torch record\n");
            bad = 1;
        }
    }

    /* --- D-89: the static records' per-frame row pass (1000:D860) ---------
     * The placed humans' type-2 row carries the settle hook (0xFF), so the
     * row pass lands them on the surface every frame: the tile walk's
     * row*16-7 becomes row*16-8 once the world runs.  The acting record (0)
     * gets the same value from its own command path (1000:E4B0), so the two
     * paths must agree; the free slots (x = 0xFFFF) must be skipped. */
    {
        static const uint32_t wantY[4] = { 24u, 24u, 136u, 264u };
        level_load(0);
        level_begin();
        for (uint32_t i = 0; i < 4000u && level_starting(); i++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        for (uint32_t i = 0; i < 4u; i++) {
            const uint8_t *r = object_slot(i);
            uint32_t y = (uint32_t)(rd_le_u16(r + 2));

            if (y != wantY[i]) {
                fprintf(stderr, "FAIL D-89: record %u y = %u, want %u\n", i, y,
                        wantY[i]);
                bad = 1;
            }
        }
        for (uint32_t i = 4u; i < 8u; i++) {
            const uint8_t *r = object_slot(i);
            if (r[0] != 0xFFu || r[1] != 0xFFu) {
                fprintf(stderr, "FAIL D-89: free slot %u was touched\n", i);
                bad = 1;
            }
        }
    }

    /* --- D-95: the goal scan (1000:C2CE) and the goal pan (1000:3540) ----- */
    if (level_load(6) == 0) {
        /* the map's one 0xBA cell is (28, 17); the class-9 live records are
         * 14/15/16 at (400, 0/16/32).  A type-0x18 record has no settle hook
         * and a bare RETURN tail, so it stays on the cell. */
        level_begin();
        for (uint32_t i = 0; i < 4000u && level_starting(); i++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        object_goal_scan();
        if (object_goal_flag()) {
            fprintf(stderr, "FAIL D-95: the goal flag is set with nothing on "
                            "the 0xBA cell\n");
            bad = 1;
        }
        {
            uint8_t *r = object_slot(4);

            r[0] = 0xC0u; /* x = 448 -> (448+8)/16 = 28 */
            r[1] = 0x01u;
            r[2] = 0x08u; /* y = 264 -> (264+12)/16 = 17 */
            r[3] = 0x01u;
            r[4] = 0x10u; /* frame */
            r[5] = 0x00u;
            r[6] = 0x00u; /* active */
            r[7] = 0x00u;
            r[8] = 0x18u; /* type 0x18: no row hook, a bare-RET tail */
            r[9] = 0x00u;
        }
        object_goal_scan();
        if (!object_goal_flag()) {
            fprintf(stderr, "FAIL D-95: a record on the 0xBA cell does not "
                            "reach the level's goal byte\n");
            bad = 1;
        }
        /* 0x6549 excludes id 3 (mask byte 0 = 0x1F) */
        object_slot(4)[8] = 0x03u;
        object_goal_scan();
        if (object_goal_flag()) {
            fprintf(stderr, "FAIL D-95: a 0x6549-excluded id was counted\n");
            bad = 1;
        }
        object_slot(4)[8] = 0x18u;

        /* the pan: the follower leaves the acting record and converges on the
         * first class-9 record.  Records 14/15 carry a row tail that would
         * move them, so the pan is pointed at 16 (type 0x59, no row) at
         * (400, 32): the window clamps to (0xC0, 0). */
        object_set_slot_class_for_probe(5u, 0u); /* record 14 */
        object_set_slot_class_for_probe(6u, 0u); /* record 15 */
        {
            uint32_t t, markerAt = 0xFFFFFFFFu, doneAt = 0xFFFFFFFFu;
            uint32_t holdX = 0xFFFFFFFFu, holdY = 0xFFFFFFFFu;

            for (t = 0; t < 400u && !object_marker_mode(); t++) {
                level_tick();
                palette_tick();
            }
            markerAt = t;
            holdX = level_view_x();
            holdY = level_view_y();
            /* the tail clears [0x2D8C]; the follower then leaves the goal
             * window, which is the pan's end */
            for (; t < 400u && level_view_x() == holdX; t++) {
                level_tick();
                palette_tick();
            }
            doneAt = t;
            if (holdX != 0xC0u || holdY != 0x00u) {
                fprintf(stderr, "FAIL D-95: the pan held at (%u, %u)\n", holdX,
                        holdY);
                bad = 1;
            }
            if (markerAt == 0xFFFFFFFFu) {
                fprintf(stderr, "FAIL D-95: the pan never raised [0x6531]\n");
                bad = 1;
            } else if (markerAt < 0x23u) {
                fprintf(stderr,
                        "FAIL D-95: the hold is %u frames, want >= 0x23\n",
                        markerAt);
                bad = 1;
            }
            if (doneAt > 399u || doneAt < markerAt + 0x1Eu) {
                fprintf(stderr,
                        "FAIL D-95: the tail is %u frames, want "
                        "0x1E\n",
                        doneAt - markerAt);
                bad = 1;
            }
        }
        /* the flag falls -> [0x6531] is cleared again (1000:35A2) */
        object_slot(4)[0] = 0xFFu;
        object_slot(4)[1] = 0xFFu;
        object_goal_scan();
        level_tick();
        if (object_marker_mode()) {
            fprintf(stderr, "FAIL D-95: [0x6531] survived the goal flag\n");
            bad = 1;
        }
    }

    /* --- D-259: a pan whose goal flag falls mid-track must still finish.
     * 1000:3568 is the track loop's head, so the view is captured every
     * iteration; the blocking loop never re-reads [0x6530] (1000:3540 is only
     * reached when it returns).  A human stepping off the activity block
     * clears the flag on the next goal scan - the PARKVIEW (level 26) hang.
     * The pan target here, (8, 0), differs from the (0xC0, 0) the previous
     * block leaves in the capture, so skipping the capture cannot terminate. */
    if (level_load(25) == 0) {
        uint8_t *r = object_slot(4);
        uint32_t t;
        int32_t sawRun = 0;

        level_begin();
        for (t = 0; t < 4000u && level_starting(); t++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        /* rec 4 onto PARKVIEW's one 0xBA cell (col 2, row 13) */
        r[0] = 0x20u;
        r[1] = 0x00u;
        r[2] = 0xC4u;
        r[3] = 0x00u;
        r[4] = 0x10u;
        r[5] = 0x00u;
        r[6] = 0x00u;
        r[7] = 0x00u;
        r[8] = 0x18u; /* no row hook */
        r[9] = 0x00u;
        for (t = 0; t < 40u; t++) {
            level_tick();
            palette_tick();
            if (level_goal_pan_running_for_probe()) {
                sawRun = 1;
                break;
            }
        }
        if (!sawRun) {
            fprintf(stderr, "FAIL D-259: the pan did not start\n");
            bad = 1;
        }
        /* the human walks off the cell: the goal scan clears [0x6530] */
        r[0] = 0xFFu;
        r[1] = 0xFFu;
        object_goal_scan();
        if (object_goal_flag()) {
            fprintf(stderr, "FAIL D-259: the goal flag survived the cell\n");
            bad = 1;
        }
        for (t = 0; t < 400u && level_goal_pan_running_for_probe(); t++) {
            level_tick();
            palette_tick();
        }
        if (level_goal_pan_running_for_probe()) {
            fprintf(stderr, "FAIL D-259: the pan hung after the flag fell\n");
            bad = 1;
        }
    }

    /* --- D-90: the handled-object slots' stripe (1000:5D65) ---------------- */
    {
        static uint8_t page[VIDEO_PAGE_BYTES];
        uint16_t *s = (uint16_t *)object_span_at_for_probe(0);
        uint32_t x = 300u, seed = 100u;

        memset(page, 0, sizeof page);
        memset(s, 0, 28u);
        s[0] = 0u; /* a live record index: not free */
        s[3] = 0u; /* +6: the length */
        s[4] = 8u; /* +8: the target */
        s[5] = (uint16_t)x; /* +0xA: the stripe's x */
        s[6] = (uint16_t)seed; /* +0xC: one past the drawn rows */
        s[9] = 1u; /* +0x12: growing */
        object_span_seed_for_probe(0u, seed);
        while ((object_frame_counter_probe() & 1u) != 0u)
            object_tick(level_palette()); /* an even frame: no animation */
        object_render(page, 0, 0);
        if (page[seed * 320u + x] == 0x18u) {
            fprintf(stderr, "FAIL D-90: a zero-length stripe drew\n");
            bad = 1;
        }
        /* one odd frame grows it four rows (1000:5DD1's DX = 3 loop); the
         * animation runs in the draw pass, so the odd frame must render */
        object_tick(level_palette());
        memset(page, 0, sizeof page);
        object_render(page, 0, 0);
        if (page[seed * 320u + x] != 0x18u ||
            page[(seed + 1u) * 320u + x + 1u] != 0x18u ||
            page[(seed + 3u) * 320u + x] != 0x18u ||
            page[(seed + 4u) * 320u + x] == 0x18u) {
            fprintf(stderr, "FAIL D-90: the span stripe is not drawn\n");
            bad = 1;
        }
        /* the next odd frame reaches +8 and finishes (1000:5DE2) */
        object_tick(level_palette());
        object_tick(level_palette());
        memset(page, 0, sizeof page);
        object_render(page, 0, 0);
        if (page[(seed + 7u) * 320u + x] != 0x18u || s[9] != 0xFFFFu) {
            fprintf(stderr, "FAIL D-90: the stripe did not grow to +8\n");
            bad = 1;
        }
        /* released (1000:EA71): +8 = 0 and state 0 retract it */
        s[4] = 0u;
        s[9] = 0u;
        for (uint32_t i = 0; i < 3u; i++) {
            object_tick(level_palette()); /* even: no animation */
            object_tick(level_palette()); /* odd */
            memset(page, 0, sizeof page);
            object_render(page, 0, 0);
        }
        if (page[seed * 320u + x] == 0x18u) {
            fprintf(stderr, "FAIL D-90: the released stripe did not retract\n");
            bad = 1;
        }
        memset(s, 0xFF, 28u); /* free it again */
        object_span_seed_for_probe(0u, 0u);
    }

    /* D-157: draw_level_text clamps [0x2] to 0x78 before its page frames. */
    level_set_view(0u, 0xA8u);
    level_end_redraw_page();
    if (level_view_y() != 0x78u) {
        fprintf(stderr, "FAIL the end page view clamp: y = %u\n",
                level_view_y());
        bad = 1;
    }
    level_set_view(0u, 0u);

    /* D-121: 1000:3D5E recolours every page byte below 0x80 to 0x1E. */
    {
        uint8_t *page = video_page(0);

        page[0] = 0x00;
        page[1] = 0x7F;
        page[2] = 0x80;
        page[3] = 0xFF;
        level_claim_recolour();
        if (page[0] != 0x1Eu || page[1] != 0x1Eu || page[2] != 0x80u ||
            page[3] != 0xFFu) {
            fprintf(stderr, "FAIL the claim recolour (D-121)\n");
            bad = 1;
        }
    }

    if (!bad)
        printf(
            "level tests: OK (the 160-record census + the byte-exact maps + the render)\n");
    return bad;
}
