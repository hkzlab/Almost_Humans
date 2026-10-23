/* level.c — the level definitions (game_structure.md §1 subsystem 9,
 * re_formats.md §2): LEVELS\LA.BIN … LP.BIN, 16 files x 10 records = 160
 * levels.
 *
 * Record layout (re_formats.md §2/§2.1a, census-verified over all 160):
 *
 *   +0x00  13-byte header:
 *            u16 style       low byte = the tile style 1..6 -> DEFS\BD<n>.BIN
 *                            high byte (+0x01) = the intro-movie index /
 *                            disc selector (0xFF = no cutscene)
 *            u16 goal count  low byte = the solved-flag threshold;
 *                            high byte (+0x03) = the entity lifetime seed
 *            u16 object-test mode (+0x04)
 *            u8  unused (+0x06; 0 in all 160 records)
 *            u16 time limit A (+0x07, BCD MMSS)
 *            u16 time limit B (+0x09), C (+0x0B)
 *   goal   ASCIIZ ("\r" = a line break)
 *   author ASCIIZ
 *   6      constant signature 00 20 00 14 00 00
 *   2      varying (the editor metadata tail)
 *   640    tile map, 32 columns x 20 rows, row-major
 *
 * The level id is NOT stored in the file: load_level takes it and builds
 * the name LEVELS\L<'A'+id/10>.BIN, record id%10 (1000:30BC).
 *
 * The map start is deterministic: the author's NUL + 8 bytes. The next
 * record begins at the first candidate at or after the map's end (the
 * census walk, tools/level_census.py) — some records carry a 0/1-byte
 * alignment trailer after the map.
 */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_LEN 640u
#define MAP_COLS 32u
#define MAP_ROWS 20u
#define REC_HDR 13u
#define SIG_LEN 6u
#define FIELD_LEN 8u

static const uint8_t recSig[SIG_LEN] = { 0x00, 0x20, 0x00, 0x14, 0x00, 0x00 };

/* The 202F:0x6287-equivalent: the intro cue state lives in script.c; the
 * level's own state: */
static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static LevelData level_data_cur;
static int level_loaded;

/* --- the level's visual assets (re_formats.md §3-§5) -------------------
 *
 * DEFS\BD<style>.BIN: the defs walk — the loader steps the buffer
 * sequentially (array_max_word over the A/B offset tables, then the fixed
 * blocks); field L (the u16 background index) selects BACKS\BG<n>.RAW.
 * LEVELS\BS<style>.BLK: the 169x16x16 tile atlas behind a 0x400 APL header.
 * BACKS\BG<n>.RAW: 512x320 chunky + a 768-byte 6-bit palette; draw_map_digit
 * copies only its first 64 entries (0xC0 bytes) into the DAC. */

#define VIEW_W 320u
#define VIEW_H 200u
#define WORLD_W 512u
#define WORLD_H 320u
#define ATLAS_TILES 169u

static const OSystem *level_osys;

static uint8_t *atlas;      /* 0x400 + 169*256                            */
static uint8_t *bgImage;    /* 512*320 + 768                              */
static uint8_t *world;      /* the 512x320 composite (BG + terrain)       */

static void visuals_free(void)
{
    free(atlas); atlas = NULL;
    free(bgImage); bgImage = NULL;
    free(world); world = NULL;
}

/* The BD walk (re_formats.md §4): returns the end offset (== the file size
 * for all six shipped files), 0 on a bounds failure.  The doc's "D" row —
 * the 5 far pointers — is loader-constructed, 0 file bytes; row J advances
 * by the LITERAL si += word[si] (the length word is part of the skip), then
 * 8 bytes; the "M" count the docs tabulate is the O record's count (the
 * real M count is 0..1).  Byte-exact over BD1..BD6 (2026-10-23). */
static uint32_t bd_walk(const uint8_t *b, uint32_t size, unsigned *bgIndex)
{
    uint32_t si = 0;

    /* A / B: 256-word offset tables whose largest entry is the record's
     * size sentinel; the strides 0x10 / 0x20 pad up afterwards. */
    for (unsigned pass = 0; pass < 2u; pass++) {
        if (si + 512u > size) return 0;
        uint32_t m = 0;
        for (unsigned i = 0; i < 256u; i++) {
            uint32_t v = rd_le16(b + si + i * 2u);
            if (v > m) m = v;
        }
        si += m + (pass ? 0x20u : 0x10u);
    }

    si += 0x600u;                          /* C                            */
    si += 0x80u;                           /* E (D = 0 file bytes)         */
    if (si + 2u > size) return 0;
    unsigned n = rd_le16(b + si);
    si += 2u + 3u * n * 0x100u;            /* F + G                        */
    si += 0x200u + 0xC0u;                  /* H + I                        */
    if (si + 2u > size) return 0;
    si += rd_le16(b + si) + 8u;            /* J                            */
    si += 4u;                              /* K                            */
    if (si + 2u > size) return 0;
    *bgIndex = rd_le16(b + si);            /* L                            */
    si += 2u;
    if (si + 2u > size) return 0;
    unsigned m = rd_le16(b + si);
    si += 2u + 4u * m;                     /* M                            */
    if (si + 1u > size) return 0;
    n = b[si];
    si += 1u + 17u * n;                    /* N                            */
    if (si + 1u > size) return 0;
    n = b[si];
    si += 1u + n;                          /* O                            */
    si += 0x1000u;                         /* P                            */
    if (si + 2u > size) return 0;
    return si + 2u;                        /* Q                            */
}

/* draw_map_digit (1000:4F22) + the BD walk: the style's defs, atlas and
 * background, all off the current disc (everything ships on HUMANS.001). */
static int visuals_load(unsigned style)
{
    visuals_free();

    char name[32];
    snprintf(name, sizeof name, "DEFS\\BD%u.BIN", style);
    uint32_t bdSize = 0;
    uint16_t chk = 0;
    uint8_t *bd = res_loadChunk(name, &bdSize, &chk);
    if (!bd) return -1;
    unsigned bgIdx = 0;
    uint32_t end = bd_walk(bd, bdSize, &bgIdx);
    free(bd);
    if (end != bdSize) return -1;

    snprintf(name, sizeof name, "LEVELS\\BS%u.BLK", style);
    atlas = res_loadChunk(name, &bdSize, &chk);
    if (!atlas || bdSize != 0x400u + ATLAS_TILES * 0x100u) {
        visuals_free();
        return -1;
    }

    if (bgIdx != 0u) {
        snprintf(name, sizeof name, "BACKS\\BG%u.RAW", bgIdx);
        bgImage = res_loadChunk(name, &bdSize, &chk);
        if (!bgImage || bdSize != WORLD_W * WORLD_H + 768u) {
            visuals_free();
            return -1;
        }
    }

    world = malloc(WORLD_W * WORLD_H);
    if (!world) {
        visuals_free();
        return -1;
    }
    return 0;
}

int level_init(const OSystem *osys)
{
    level_osys = osys;         /* the render goes through videoSetPalette  */
    return 0;
}

const LevelData *level_data(void)
{
    return level_loaded ? &level_data_cur : NULL;
}

/* The level text carries literal CR and tab from the authoring tool — the
 * census's _TEXT set (tools/level_census.py); rejecting them silently
 * truncates the walk (level 9 of LA.BIN is the first casualty). */
static int isTextByte(uint8_t b)
{
    return (b >= 0x20u && b < 0x7Fu) || b == 0x09u || b == 0x0Au || b == 0x0Du;
}

/* A record plausibly begins at p: the header + the goal ASCIIZ (>= 4) +
 * the author ASCIIZ (>= 2) + the 6-byte signature.  Returns the map's
 * offset, or 0. */
static uint32_t recIsStart(const uint8_t *b, uint32_t size, uint32_t p)
{
    if (p + REC_HDR + SIG_LEN + MAP_LEN > size) return 0;
    const uint8_t *h = b + p;
    if (h[0] < 1u || h[0] > 6u) return 0;
    if (h[2] < 1u || h[2] > 3u) return 0;

    uint32_t q = p + REC_HDR;
    uint32_t goalStart = q;
    while (q < size && isTextByte(b[q])) q++;
    if (q - goalStart < 4u) return 0;
    if (q >= size || b[q] != 0u) return 0;
    q++;                                       /* the goal's NUL */

    uint32_t authorStart = q;
    while (q < size && isTextByte(b[q])) q++;
    if (q - authorStart < 2u) return 0;
    if (q >= size || b[q] != 0u) return 0;
    q++;                                       /* the author's NUL */

    if (q + SIG_LEN > size || memcmp(b + q, recSig, SIG_LEN) != 0) return 0;
    return q + FIELD_LEN;                      /* the map's start */
}

/* The next record's start: the first candidate at or after `from`. */
static uint32_t recNextStart(const uint8_t *b, uint32_t size, uint32_t from)
{
    uint32_t p = from;
    while (p + REC_HDR + SIG_LEN + MAP_LEN <= size) {
        uint32_t mo = recIsStart(b, size, p);
        if (mo) return p;
        p++;
    }
    return 0;                                  /* no further record */
}

int level_load(unsigned levelId)
{
    if (levelId >= 160u) return -1;

    unsigned disc = 1u;                        /* the L*.BIN files ship on HUMANS.001 */
    res_setDisc(disc);

    char name[32];
    snprintf(name, sizeof name, "LEVELS\\L%c.BIN", 'A' + levelId / 10u);

    uint32_t fileSize = 0;
    uint16_t chk = 0;
    uint8_t *image = res_loadChunk(name, &fileSize, &chk);
    if (!image) return -1;

    /* walk to the record levelId%10 */
    uint32_t p = 0;
    unsigned rec = 0;
    LevelData *d = &level_data_cur;
    memset(d, 0, sizeof *d);

    int ok = 0;
    while (p + REC_HDR <= fileSize) {
        uint32_t mapOff = recIsStart(image, fileSize, p);
        if (mapOff == 0) break;                /* the walk: the candidates tile the file */

        if (rec == levelId % 10u) {
            const uint8_t *h = image + p;
            d->levelId = levelId;
            d->style = h[0];
            d->introIndex = h[1];
            d->goalCount = h[2];
            d->entityLife = h[3];
            d->objectTest = rd_le16(h + 4);
            d->unused06 = h[6];
            d->timeA = rd_le16(h + 7);
            d->timeB = rd_le16(h + 9);
            d->timeC = rd_le16(h + 11);
            memcpy(d->map, image + mapOff, MAP_LEN);
            d->mapFileOff = mapOff;

            /* the goal/author: the ASCIIZ strings between the header and the SIG */
            {
                uint32_t q = p + REC_HDR;
                uint32_t n = 0;
                while (q < fileSize && image[q] != 0u && n + 1 < sizeof d->goal) {
                    d->goal[n++] = (char)image[q++];
                }
                d->goal[n] = 0;
                q++;                            /* the NUL */
                n = 0;
                while (q < fileSize && image[q] != 0u && n + 1 < sizeof d->author) {
                    d->author[n++] = (char)image[q++];
                }
                d->author[n] = 0;
            }
            ok = 1;
        }

        rec++;
        p = recNextStart(image, fileSize, mapOff + MAP_LEN);
        if (p == 0) break;                     /* the file's records exhausted */
    }

    free(image);
    if (!ok) return -1;

    /* the header's style selects the visual set; the census across all 160
     * levels doubles as the BD-walk byte-exactness check (every style 1..6
     * walks to EOF or the load fails) */
    if (visuals_load(d->style) != 0) return -1;

    level_loaded = 1;
    return 0;
}

const uint8_t *level_map(void)
{
    return level_loaded ? level_data_cur.map : NULL;
}

/* The in-level per-tick render (draw_map_digit 1000:4F22 + draw_terrain
 * 1000:4FEF): the BG fills the 512x320 screen buffer, draw_terrain blits the
 * map's tiles over it (tile ids >= 0xA9 are empty and let the BG through;
 * every non-zero tile byte gains the +0x20 bank bias, zero stays 0), and the
 * visible 320x200 window reads the buffer's left edge (the CRTC start at
 * scroll 0,0).  The palette: the BG's first 64 entries only.  The entity /
 * logic systems land with entity.c.  Returns 1 while the level is running. */
int level_tick(void)
{
    if (!level_loaded) return 0;
    if (!world || !level_osys) return 1;

    if (bgImage) memcpy(world, bgImage, WORLD_W * WORLD_H);
    else memset(world, 0, WORLD_W * WORLD_H);

    for (unsigned r = 0; r < MAP_ROWS; r++) {
        for (unsigned c = 0; c < MAP_COLS; c++) {
            unsigned tile = level_data_cur.map[r * MAP_COLS + c];
            if (tile >= 0xA9u) continue;
            const uint8_t *src = atlas + 0x400u + tile * 0x100u;
            uint8_t *dst = world + (r * 16u) * WORLD_W + c * 16u;
            for (unsigned y = 0; y < 16u; y++) {
                for (unsigned x = 0; x < 16u; x++) {
                    uint8_t v = src[y * 16u + x];
                    dst[y * WORLD_W + x] = v ? (uint8_t)(v + 0x20u) : 0u;
                }
            }
        }
    }

    uint8_t *page = level_osys->videoPage(0);
    for (unsigned y = 0; y < VIEW_H; y++)
        memcpy(page + y * VIEW_W, world + y * WORLD_W, VIEW_W);

    /* draw_map_digit: the first 64 entries only (the 0xC0-byte copy) */
    if (bgImage) level_osys->videoSetPalette(bgImage + WORLD_W * WORLD_H, 0, 64);
    return 1;
}
