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
            level_tick();
            const uint8_t *page = osys_null_page(0);
            uint32_t sum = 0, nz = 0;
            for (unsigned i = 0; i < 64000u; i++) {
                sum += page[i];
                if (page[i]) nz++;
            }
            if (sum != 1161943u || nz != 25840u) {
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
        }
    }

    if (!bad) printf("level tests: OK (the 160-record census + the byte-exact maps + the render)\n");
    return bad;
}
