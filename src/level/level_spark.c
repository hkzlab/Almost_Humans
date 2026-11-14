#include "level/level.h"
#include "level/level_internal.h"
#include "object/object.h"
#include "osystem.h"
#include "statusbar.h"
#include "video.h"

#include <stdint.h>
#include <string.h>

static struct {
    uint8_t live, value;
    uint16_t x, y, age;
} _spark[0x58];

/* --- the claim's particles (1000:3E73 init, 0x3FB3 step, 0x3F4A flush) ------
 * The list at 0x48BC is 0x58 slots of 8 bytes {live, value, x, y, age}.  The
 * tables are the image's: the age -> direction walk at 0x1B03, the eight
 * directions at 0x1B56 and the spawn nibbles at 0x1B76. */
static const uint8_t _spark_path[83] = {
    0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x01u, 0x01u,
    0x01u, 0x01u, 0x02u, 0x01u, 0x02u, 0x02u, 0x01u, 0x02u, 0x02u, 0x02u, 0x02u,
    0x02u, 0x02u, 0x03u, 0x02u, 0x02u, 0x03u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u,
    0x03u, 0x04u, 0x03u, 0x04u, 0x04u, 0x03u, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u,
    0x04u, 0x05u, 0x04u, 0x04u, 0x05u, 0x04u, 0x05u, 0x05u, 0x05u, 0x05u, 0x05u,
    0x06u, 0x05u, 0x06u, 0x06u, 0x05u, 0x06u, 0x06u, 0x06u, 0x06u, 0x06u, 0x06u,
    0x07u, 0x06u, 0x06u, 0x07u, 0x06u, 0x07u, 0x07u, 0x07u, 0x07u, 0x07u, 0x00u,
    0x07u, 0x00u, 0x00u, 0x07u, 0x00u, 0x00u,
};

static const uint8_t _spark_stream[256] = {
    0x88u, 0x45u, 0xE1u, 0x08u, 0x6Bu, 0xDBu, 0x63u, 0xC4u, 0x3Bu, 0x8Eu, 0xFEu,
    0xA0u, 0xD8u, 0x1Eu, 0xA6u, 0x26u, 0xBBu, 0x32u, 0x67u, 0xF4u, 0x4Fu, 0x81u,
    0xC9u, 0x4Du, 0x8Bu, 0x61u, 0xF8u, 0x0Cu, 0x5Bu, 0x11u, 0x65u, 0xA2u, 0x38u,
    0x13u, 0x78u, 0xACu, 0x19u, 0xC1u, 0x6Eu, 0x94u, 0x4Au, 0xCDu, 0x74u, 0x2Eu,
    0x52u, 0x0Fu, 0x47u, 0xE9u, 0x92u, 0x7Cu, 0x2Au, 0x34u, 0x85u, 0xA9u, 0x31u,
    0xCBu, 0x1Bu, 0xB2u, 0xAEu, 0xE6u, 0x02u, 0x43u, 0xEBu, 0xB3u, 0x59u, 0xE3u,
    0x9Fu, 0x55u, 0xB8u, 0x0Au, 0x76u, 0xDCu, 0x05u, 0x56u, 0xD0u, 0x5Fu, 0xB6u,
    0x25u, 0xCAu, 0x21u, 0xA7u, 0x6Du, 0xFAu, 0x58u, 0x5Du, 0x9Du, 0x28u, 0xC8u,
    0x72u, 0x3Cu, 0xF9u, 0xA4u, 0x4Bu, 0x0Eu, 0x44u, 0x93u, 0xCCu, 0x64u, 0x50u,
    0xD2u, 0x3Eu, 0x98u, 0xC7u, 0x69u, 0x04u, 0xE8u, 0x77u, 0x8Au, 0x12u, 0x46u,
    0xC3u, 0x7Au, 0x2Du, 0x79u, 0x22u, 0x8Cu, 0xDEu, 0xFDu, 0x5Au, 0x42u, 0xA5u,
    0x29u, 0xAAu, 0xC5u, 0xA1u, 0x00u, 0x99u, 0x3Du, 0xD9u, 0xB1u, 0x24u, 0x5Cu,
    0xB5u, 0x3Fu, 0x80u, 0xFFu, 0x48u, 0x96u, 0x83u, 0x16u, 0x49u, 0x89u, 0xABu,
    0xD1u, 0x1Du, 0x41u, 0x68u, 0xFCu, 0x36u, 0xB7u, 0x73u, 0xD6u, 0x39u, 0xF6u,
    0x03u, 0x14u, 0x53u, 0xA8u, 0xFBu, 0xDAu, 0x57u, 0x66u, 0x23u, 0xD7u, 0x60u,
    0xC0u, 0x07u, 0x2Bu, 0x75u, 0x3Au, 0xEAu, 0xD5u, 0xF2u, 0x87u, 0x09u, 0x37u,
    0x27u, 0xBFu, 0x40u, 0x9Bu, 0x6Fu, 0x1Fu, 0x33u, 0x6Au, 0xDFu, 0x70u, 0x01u,
    0x62u, 0x86u, 0x2Fu, 0xB9u, 0x82u, 0x90u, 0x06u, 0x91u, 0x51u, 0xB4u, 0xEFu,
    0xE5u, 0xC2u, 0xE0u, 0x54u, 0x0Bu, 0x0Du, 0x15u, 0xD4u, 0xF1u, 0x7Du, 0x35u,
    0x10u, 0x84u, 0x7Eu, 0x8Fu, 0x1Au, 0x6Cu, 0xD3u, 0xF7u, 0x30u, 0xB0u, 0x5Eu,
    0x17u, 0x8Du, 0xC6u, 0x20u, 0x1Cu, 0xA3u, 0xE2u, 0x18u, 0x2Cu, 0xF0u, 0xBAu,
    0x97u, 0x71u, 0x7Fu, 0xAFu, 0x4Cu, 0x7Bu, 0x9Eu, 0xF5u, 0xCFu, 0x4Eu, 0x95u,
    0xEDu, 0xBCu, 0xADu, 0xEEu, 0xCEu, 0xF3u, 0xECu, 0xE4u, 0xDDu, 0xE7u, 0xBEu,
    0x9Au, 0xBDu, 0x9Cu,
};
static const int8_t _spark_dir[8][2] = {
    { 1, 0 },  { 1, -1 }, { 0, -1 }, { -1, -1 },
    { -1, 0 }, { -1, 1 }, { 0, 1 },  { 1, 1 },
};
static uint8_t _spark_write;
static uint16_t _spark_spawned; // [0x1AF8] cursor, [0x1AFC] count
static uint16_t _spark_ox, _spark_oy; // [0x1AFE] / [0x1B00]
static uint8_t _spark_shade[16][16]; // 0x19EA, from the recipe sprite
static uint8_t _spark_on;
/* 1000:3DC0's scratch strip (A000:F000): 0x12 addresses x 0x40 rows, the four
 * plane bytes of each.  Saved from the page at (spark origin - 0x20) and
 * restored over it before every particle frame; an expired cell paints its
 * byte into it, so the pixel survives the restore (D-132). */
static uint8_t _spark_strip[0x40][0x12][4];
static int16_t _spark_strip_x, _spark_strip_y; // [0x1AEA] / [0x1AEC]
static uint8_t _spark_strip_on;

/** @brief 1000:3DC0: save the 0x12-address x 0x40-row page strip at
 * (ox, oy) into the scratch buffer, four plane bytes per address.
 *
 * The original copies with Map Mask 0x0F and Bit Mask 0, i.e. each word is the
 * freshly read latch, so the strip is an exact copy of the four planes.
 *
 * @param ox the strip origin x
 * @param oy the strip origin y
 */
static void _spark_strip_save(int16_t ox, int16_t oy)
{
    int32_t vx = (int32_t)level_view_x(), vy = (int32_t)level_view_y();

    _spark_strip_x = ox;
    _spark_strip_y = oy;
    _spark_strip_on = 1;
    for (uint16_t row = 0; row < 0x40u; row++)
        for (uint16_t col = 0; col < 0x12u; col++) {
            int32_t sx = _spark_strip_x - vx + (int32_t)col * 4;
            int32_t sy = _spark_strip_y - vy + (int32_t)row;

            if (sx >= 0 && sx < (int32_t)VIDEO_PAGE_W && sy >= 0 &&
                sy < (int32_t)VIDEO_PAGE_H_MODE13H) {
                const uint8_t *px = video_vram_group(
                    (uint16_t)((uint32_t)sy * 88u + ((uint32_t)sx >> 2)));

                for (uint16_t p = 0; p < 4u; p++)
                    _spark_strip[row][col][p] = px[p];
            } else {
                for (uint16_t p = 0; p < 4u; p++)
                    _spark_strip[row][col][p] = 0u;
            }
        }
}

/** @brief 1000:3E29: copy the strip back over the page region it was saved
 * from (the first step of every particle frame).
 */
static void _spark_strip_restore(void)
{
    int32_t vx = (int32_t)level_view_x(), vy = (int32_t)level_view_y();

    if (!_spark_strip_on) return;
    for (uint16_t row = 0; row < 0x40u; row++)
        for (uint16_t col = 0; col < 0x12u; col++) {
            int32_t sx = _spark_strip_x - vx + (int32_t)col * 4;
            int32_t sy = _spark_strip_y - vy + (int32_t)row;

            if (sx < 0 || sx >= (int32_t)VIDEO_PAGE_W || sy < 0 ||
                sy >= (int32_t)VIDEO_PAGE_H_MODE13H)
                continue;
            {
                uint8_t *px = video_vram_group(
                    (uint16_t)((uint32_t)sy * 88u + ((uint32_t)sx >> 2)));

                for (uint16_t p = 0; p < 4u; p++)
                    px[p] = _spark_strip[row][col][p];
            }
        }
}

/** @brief 1000:4073: an expired cell's plane byte into the strip, so the next
 * restore keeps it on the page for the rest of the claim.
 *
 * @param wx the cell's world x
 * @param wy the cell's world y
 * @param value the shade byte (0 is not stored)
 */
static void _spark_paint(uint32_t wx, uint32_t wy, uint8_t value)
{
    int32_t dx = (int32_t)wx - _spark_strip_x;
    int32_t dy = (int32_t)wy - _spark_strip_y;
    uint32_t col, row, plane;

    if (!_spark_strip_on || value == 0u) return;
    if (dx < 0 || dy < 0) return;
    col = (uint32_t)dx >> 2;
    row = (uint32_t)dy;
    plane = (uint32_t)dx & 3u;
    if (col >= 0x12u || row >= 0x40u) return;
    _spark_strip[row][col][plane] = value;
}

void level_spark_begin(uint16_t ox, uint16_t oy, const uint8_t *shade)
{
    memset(_spark, 0, sizeof _spark);
    if (shade) memcpy(_spark_shade, shade, sizeof _spark_shade);
    _spark_write = 0u;
    _spark_spawned = 0u;
    _spark_ox = ox;
    _spark_oy = oy;
    _spark_on = 1;
    /* 1000:3E73's tail saves the strip from the presented page - here, the
     * claim's frozen black page plus the two records it drew last frame. */
    _level_page_bake();
    /* 1000:3E73's tail: the strip origin is (origin - 0x20) in both axes. */
    _spark_strip_save((int16_t)((int32_t)ox - 0x20),
                      (int16_t)((int32_t)oy - 0x20));
}

uint8_t level_spark_active(void) { return _spark_on; }

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint8_t level_spark_strip_at(uint32_t col, uint32_t row, uint32_t plane)
{
    if (col >= 0x12u || row >= 0x40u || plane >= 4u) return 0u;
    return _spark_strip[row][col][plane];
}
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint8_t level_spark_byte(uint32_t off)
{ return off < sizeof _spark ? ((const uint8_t *)_spark)[off] : 0u; }
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void level_spark_set_byte(uint32_t off, uint8_t value)
{
    if (off < sizeof _spark) ((uint8_t *)_spark)[off] = value;
}
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
int32_t level_spark_peek(uint32_t *x, uint32_t *y, uint32_t *value)
{
    for (uint16_t i = 0; i < 0x58u; i++)
        if (_spark[i].live) {
            if (x) *x = _spark[i].x;
            if (y) *y = _spark[i].y;
            if (value) *value = _spark[i].value;
            return 1;
        }
    return 0;
}
#endif /* HUMANSRE_TEST_PROBES */

/** @brief 1000:3FB3: spawn one cell (while fewer than 0x100 have been spawned and the
 * write slot is free), then move every live cell by path[age] and drop it at
 * age 0x53.  Stops when the spawns are done and nothing is live ([0x1B02]).
 */
static void _spark_step(void)
{
    uint8_t live = 0;

    if (!_spark_on) return;
    if (_spark_spawned < 0x100u && !_spark[_spark_write].live) {
        uint8_t b = _spark_stream[_spark_spawned & 0xFFu];
        uint8_t dx = (uint8_t)(b & 0x0Fu), dy = (uint8_t)(b >> 4);

        _spark[_spark_write].live = 1;
        _spark[_spark_write].value = _spark_shade[dy][dx];
        _spark[_spark_write].x = (uint16_t)(_spark_ox + dx);
        _spark[_spark_write].y = (uint16_t)(_spark_oy + dy);
        _spark[_spark_write].age = 0u;
        _spark_write = (_spark_write + 1u) % 0x58u;
        _spark_spawned++;
    }
    for (uint16_t i = 0; i < 0x58u; i++) {
        uint8_t d;

        if (!_spark[i].live) continue;
        live = 1;
        d = _spark_path[_spark[i].age] & 7u;
        _spark[i].x = (uint16_t)(_spark[i].x + _spark_dir[d][0]);
        _spark[i].y = (uint16_t)(_spark[i].y + _spark_dir[d][1]);
        if (++_spark[i].age >= 0x53u) {
            _spark[i].live = 0;
            _spark_paint(_spark[i].x, _spark[i].y, _spark[i].value);
        }
    }
    if (_spark_spawned >= 0x100u && !live) _spark_on = 0;
}

/** @brief `1000:3F4A`'s per-frame particle draw, in the flat model: `DI = y*0x58 +
 * (x>>2) + [0x5AC4]`, plane `x & 3`, one `STOSB` of the cell's shade per cell.
 * The original writes into its 512x320 page with no clip; the flat page is
 * 320x200, so this clips to the window.  A plane byte is one pixel colour under
 * the port's `4*address + plane` mapping (D-04), so the write is a full byte,
 * not a bit scatter.  `0x4073` (the expired cell's paint into the 0x12-wide
 * scratch strip) is a separate routine - see D-132.
 */
static void _spark_draw(void)
{
    int32_t vx = (int32_t)level_view_x(), vy = (int32_t)level_view_y();

    for (uint16_t i = 0; i < 0x58u; i++) {
        int32_t sx, sy;
        uint32_t plane, addr;

        if (!_spark[i].live || _spark[i].value == 0u) continue;
        sx = (int32_t)_spark[i].x - vx;
        sy = (int32_t)_spark[i].y - vy;
        if (sx < 0 || sx >= (int32_t)VIDEO_PAGE_W || sy < 0 ||
            sy >= (int32_t)VIDEO_PAGE_H_MODE13H)
            continue;
        plane = (uint32_t)sx & 3u;
        addr = (uint32_t)sy * 88u + ((uint32_t)sx >> 2);
        /* 1000:3F9C MOV AL,[SI+1] / STOSB: the whole shade byte into the
         * plane selected by Map Mask 1 << (x & 3). */
        video_vram_group((uint16_t)addr)[plane] = _spark[i].value;
    }
}

void level_claim_recolour(void)
{
    uint8_t *page = video_page(0);

    if (!page) return;
    for (uint16_t i = 0; i < VIDEO_PAGE_BYTES; i++)
        if (page[i] < 0x80u) page[i] = 0x1Eu;
    for (uint16_t i = 0; i < VIDEO_PAGE_H_MODE13H * 88u; i++)
        for (uint16_t p = 0; p < 4u; p++)
            if (video_vram_group(i)[p] < 0x80u) video_vram_group(i)[p] = 0x1Eu;
}

void level_claim_frame(void)
{
    uint8_t *page;

    if (!_level_is_loaded() || !g_osys) return;
    page = g_osys->videoPage(0);
    if (!page) return;
    _level_spark_frame(); // 1000:3F4A / 1000:40CA's restore
    _level_page_present();
    if (object_sequence_claim_anim_active()) // 1000:40CA's two draw_object_entry
        object_render_claim_draw_records(page, (int32_t)level_view_x(),
                                         (int32_t)level_view_y());
    statusbar_tick(); // the bar band: outside 1000:3D5E's page range
}

/** @brief 1000:3F4A's per-frame particle pass: restore the strip, spawn
 * and age the cells, then draw them.
 */
void _level_spark_frame(void)
{
    if (!_spark_on) return;
    _spark_strip_restore(); // 1000:3E29
    _spark_step(); // 1000:3FB3 (spawns, ages, paints)
    _spark_draw(); // 1000:3F4A's inline loop
}
