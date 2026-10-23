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

int level_init(const OSystem *osys)
{
    (void)osys;                    /* the level loads through res.c directly */
    return 0;
}

const LevelData *level_data(void)
{
    return level_loaded ? &level_data_cur : NULL;
}

/* The level text carries literal CR and tab from the authoring tool. */
static int isTextByte(uint8_t b)
{
    return b >= 0x20u && b < 0x7Fu;
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
    level_loaded = 1;
    return 0;
}

const uint8_t *level_map(void)
{
    return level_loaded ? level_data_cur.map : NULL;
}

/* The in-level per-tick: skeleton — the entity/logic systems land with
 * entity.c.  Returns 1 while the level is running. */
int level_tick(void)
{
    return level_loaded;
}
