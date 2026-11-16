#include "object_internal.h"
#include "object_spawn.h"
#include "level/level.h"
#include "byteorder.h"
#include <string.h>

/* 0x32B9: the 0xA9 background-sprite records (12 bytes each), filled by that
 * tile handler (1000:5206); the settle's 1000:DF2C reads them. */
static struct {
    uint16_t x, y;
    uint16_t dst; // +4: the first of the five A9 log records (slot index)
    /* +8: the first map cell of the pair 1000:D5E4 rewrites.  0xFFFF means
     * the pair is outside the map and the write is dropped (D-270). */
    uint16_t mark;
} _obj_bg[32];

/* forward declarations */
static int32_t _obj_spawn_place_human(uint32_t col, uint32_t row);
static void _obj_spawn_append_spear(uint32_t col, uint32_t row);
static void _obj_spawn_bb(uint32_t col, uint32_t row);
static void _obj_spawn_be(uint32_t col, uint32_t row);
static void _obj_spawn_join_bf(uint32_t col);
static void _obj_spawn_c0(uint32_t col, uint32_t row);
static void _obj_spawn_c1(uint32_t col, uint32_t row);
static void _obj_spawn_write_pair(uint32_t tile, uint32_t x, uint32_t y);
static void _obj_spawn_b6(uint32_t col, uint32_t row);
static void _obj_spawn_b7(const uint8_t *map, uint32_t col, uint32_t row);
static void _obj_spawn_b8(uint32_t col, uint32_t row);
static uint32_t _obj_spawn_style_nudge(const uint8_t *map, uint32_t col,
                                       uint32_t row);
static void _obj_spawn_append_style(const uint8_t *map, uint32_t col,
                                    uint32_t row, uint32_t frame,
                                    uint32_t behaviour, uint32_t f8,
                                    uint32_t fc, uint32_t fe);
static void _obj_spawn_append_row9(uint8_t *map, uint32_t col, uint32_t row);
static void _obj_spawn_append_decoration(uint32_t x, uint32_t y, uint8_t tile);
static void _spawn_terrain_tile(uint8_t *map, uint32_t tile);

uint8_t _obj_bg_count;


/* 202F:0x1EB3, the tile ids the walk matches, in table order. */
static const uint8_t _obj_tiles[25] = {
    0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1,
    0xB2, 0xB3, 0xB4, 0xB5, 0xC0, 0xB6, 0xB7, 0xB8, 0xBA,
    0xBB, 0xBE, 0xBF, 0xC1, 0xF0, 0xF1, 0xF2,
};


/* 0xBE / 0xBF's hand-off: 0xBE keeps the record it appended (`0x1F1C`) and the
 * slot 0xBF joins into (`0x1F1E`); the port keeps the record index. */
static int8_t _obj_join_slot = -1;

static const uint8_t _obj_foot_a[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x2C, 0x2C, 0x2C, 0x2C,
    0x2C, 0x2C, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x50,
};

static const uint8_t _obj_foot_b[32] = {
    0x08, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x2C, 0x2C, 0x2C,
    0x2C, 0x2C, 0x2C, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x50, 0x50,
    0x50, 0x50, 0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};


/**
 * @brief 1000:524B (tile 0xAA): the human, into the first free static slot.
 *
 * @param col the column
 * @param row the row
 * @return the placed slot index, or -1 when full
 */
static int32_t _obj_spawn_place_human(uint32_t col, uint32_t row)
{
    uint16_t y =
        (uint16_t)(row * 16u + _obj_spawn_style_nudge(NULL, col, row) - 7u);

    for (uint16_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        if (!(r[6] & 0x80u)) continue;
        wr_le_u16(r + 0u, (uint16_t)(col * 16u));
        wr_le_u16(r + 2u, y); // 0x524B: nudge, then y = row*16 - 7
        wr_le_u16(r + 4u, 0x0010u);
        wr_le_u16(r + 6u, 0u);
        wr_le_u16(r + 8u, 0x0002u);
        _obj_placed_humans++;
        return (int32_t)i;
    }
    return -1;
}


/**
 * @brief 1000:527D (tile 0xAB): the spear, appended at the live end.
 *
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_append_spear(uint32_t col, uint32_t row)
{
    uint8_t *r;
    uint32_t probe;
    int32_t bx;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u,
              (uint16_t)(row * 16u + _obj_spawn_style_nudge(NULL, col, row)));
    wr_le_u16(r + 4u, 0x0060u);
    wr_le_u16(r + 6u, 0u);
    wr_le_u16(r + 8u, 0xFFFFu);
    wr_le_u16(r + 0xAu, 0u);
    _obj_slot_class[_obj_live_count - 7u] = 5u; // [0x3E5F + count-8] = 0x05
    _obj_live_count++;
    /* 1000:527D ends JMP 0x5639: the shared ground snap.  The listing stores
     * `bx - 0x10` unconditionally, so a blocked probe parks the record at
     * y = 0xFFEF. */
    bx = _obj_core_step_probe_at(r, 8, 0x10, &probe);
    wr_le_u16(r + 2u, (uint16_t)(bx - 0x10));
}


/** @brief 1000:54D9 (0xBB): {x, y, +4 = 0x26, +6 = 0, +8 = the map cell},
 * behaviour 9.  The original's `+8` is `SI - 1` (the map pointer); the port
 * stores the matched cell index (D-123).
 *
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_bb(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u,
              (uint16_t)(row * 16u + _obj_spawn_style_nudge(NULL, col, row)));
    wr_le_u16(r + 4u, 0x0026u);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, (uint16_t)(row * OBJ_COLS + col));
    _obj_slot_class[_obj_live_count - 7u] = 9u;
    _obj_live_count++;
}


/** @brief 1000:54FC (0xBE): {x, y, +4 = 0x82, +6 = 0, +8 = 0, +0xA = x, +0xE = 1},
 * behaviour 2 - and it remembers the record for 0xBF.
 *
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_be(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u,
              (uint16_t)(row * 16u + _obj_spawn_style_nudge(NULL, col, row)));
    wr_le_u16(r + 4u, 0x0082u);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, 0x0000u);
    wr_le_u16(r + 0xAu, (uint16_t)(col * 16u));
    wr_le_u16(r + 0xEu, 0x0001u);
    _obj_slot_class[_obj_live_count - 7u] = 2u;
    _obj_join_slot =
        (int8_t)(_obj_live_count + 1u); // the record, not the count
    _obj_live_count++;
}


/**
 * @brief 1000:552B (0xBF): the join half - written into the record 0xBE made.
 *
 * @param col the column
 */
static void _obj_spawn_join_bf(uint32_t col)
{
    if (_obj_join_slot < 0) return;
    wr_le_u16(_obj_array + (uint32_t)_obj_join_slot * OBJ_STRIDE + 0xCu,
              (uint16_t)(col * 16u));
}


/** @brief 1000:5535 / 1000:553D (0xC0 / 0xC1): 0xC0 runs the human handler and keeps
 * its slot as the player's record ([0x180A]); 0xC1 appends {x, y-0x10,
 * +4 = 0x100}, behaviour 0.
 *
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_c0(uint32_t col, uint32_t row)
{
    int32_t slot = _obj_spawn_place_human(col, row);

    /* 1000:524B leaves SI = 0x3F05 (record 8, the marker) when all eight
     * static slots are full, and 1000:5538 stores SI in [0x180A] regardless.
     * Unreachable in the 160 shipped levels (the worst 0xC0 map needs all
     * eight), but the store has no guard in the listing. */
    _obj_player = slot >= 0 ? (int16_t)(slot) : (int32_t)OBJ_LIVE0;
}


/**
 * @brief Spawn the C1 decoration at (col, row).
 *
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_c1(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(
        r + 2u,
        (uint16_t)(row * 16u + _obj_spawn_style_nudge(NULL, col, row) - 0x10u));
    wr_le_u16(r + 4u, 0x0100u);
    wr_le_u16(r + 6u, 0x0000u);
    _obj_slot_class[_obj_live_count - 7u] = 0u;
    _obj_live_count++;
}


/**
 * @brief 1000:5310 / 1000:532F / 1000:5338 / 1000:5357: the four data-only
 * tiles fill the two 0x649A / 0x64AE TER slots.
 *
 * 0xAF (and 0xB1) stores the cell in the slot's current and primary-target
 * words (+0/+2, +4/+6) and zeroes +0xC..+0x12 - which includes the +0x10
 * disable word, so the slot draws and the 0xC7A0 patrol starts.  It does NOT
 * touch +8/+0xA.  0xB0 (and 0xB2) writes +8/+0xA of the same slot: the
 * secondary target the patrol moves to while +0x10 is zero.
 *
 * @param tile the tile byte
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _obj_spawn_write_pair(uint32_t tile, uint32_t x, uint32_t y)
{
    uint8_t *e = _obj_ter_slot[(tile == 0xB1u || tile == 0xB2u) ? 1u : 0u];

    if (tile == 0xAFu || tile == 0xB1u) {
        wr_le_u16(e + 0u, (uint16_t)x); // 1000:5313/0x533B
        wr_le_u16(e + 2u, (uint16_t)y);
        wr_le_u16(e + 4u, (uint16_t)x);
        wr_le_u16(e + 6u, (uint16_t)y);
        for (uint16_t i = 0xCu; i <= 0x12u; i += 2u) // 1000:531E..0x532B
            wr_le_u16(e + i, 0u);
    } else {
        wr_le_u16(e + 8u, (uint16_t)x); // 1000:5332/0x535A
        wr_le_u16(e + 0xAu, (uint16_t)y);
    }
}


/** @brief 1000:53BA (0xB6): a placed object in the first *static* slot - frame 0xFE,
 * type 0x1D, +0xA = 0xFA, +0xC = the record-area pointer - yet the live count
 * still grows.  The pointer is the next live record, stored in the same
 * `record - _obj_array + 4` form as the other links.
 *
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_b6(uint32_t col, uint32_t row)
{
    for (uint16_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;

        if (!(r[6] & 0x80u)) continue;
        wr_le_u16(r + 0u, (uint16_t)(col * 16u));
        wr_le_u16(r + 2u,
                  (uint16_t)(row * 16u +
                             _obj_spawn_style_nudge(NULL, col, row) - 0x10u));
        wr_le_u16(r + 4u, 0x00FEu);
        wr_le_u16(r + 6u, 0x0000u);
        wr_le_u16(r + 8u, 0x001Du);
        wr_le_u16(r + 0xAu, 0x00FAu);
        wr_le_u16(
            r + 0xCu,
            (uint16_t)(((_obj_live_count + 1u) * OBJ_STRIDE + 4u) & 0xFFFFu));
        _obj_slot_class[_obj_live_count - 7u] = 0u;
        _obj_live_count++;
        return;
    }
}


/** @brief 1000:53FD (0xB7): TWO appended records; the first carries the only computed
 * class (`+6 = (count - 6) << 8`), behaviour 8, and the 0xB6 neighbour patch:
 * cells -5..-2 -> frame 0x116 / +0xA += 2, cells 0..+3 -> 0x11A / +3, none ->
 * the initial 0xD4 / 0.  The left group is tested first.  The second record is
 * empty, behaviour 0x85.
 *
 * @param map the 640-cell map
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_b7(const uint8_t *map, uint32_t col, uint32_t row)
{
    uint8_t *r;
    uint16_t cell;
    uint8_t variant = 0; // 0 none, 1 left, 2 right

    if (_obj_live_count + 2u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    cell = (uint16_t)(row * OBJ_COLS + col);
    if (map) {
        static const int32_t left[4] = { -5, -4, -3, -2 };
        static const int32_t right[4] = { 0, 1, 2, 3 };

        for (uint16_t k = 0; k < 4u && !variant; k++) {
            int64_t c = (int64_t)cell + left[k];

            /* 1000:541D..0x5431: [SI-5..SI-2], no range test (D-270). */
            if (_obj_core_map_byte_at(map, (int32_t)c) == 0xB6u) variant = 1;
        }
        for (uint16_t k = 0; k < 4u && !variant; k++) {
            int64_t c = (int64_t)cell + right[k];

            /* 1000:5434..0x5446: [SI]..[SI+3], no range test. */
            if (_obj_core_map_byte_at(map, (int32_t)c) == 0xB6u) variant = 2;
        }
    }
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u, (uint16_t)(row * 16u +
                                 _obj_spawn_style_nudge(NULL, col, row) - 8u));
    wr_le_u16(r + 4u, variant ? (variant == 1u ? 0x0116u : 0x011Au) : 0x00D4u);
    wr_le_u16(r + 6u, (uint16_t)(((_obj_live_count - 6u) << 8) & 0xFF00u));
    wr_le_u16(r + 8u, 0x0000u);
    wr_le_u16(r + 0xAu, variant == 1u ? 2u : (variant == 2u ? 3u : 0u));
    _obj_slot_class[_obj_live_count - 7u] = 8u;
    _obj_live_count++;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    memset(r, 0, OBJ_STRIDE);
    _obj_slot_class[_obj_live_count - 7u] = 0x85u;
    _obj_live_count++;
}


/** @brief 1000:5481 (0xB8): one appended record, frame 0xAF, behaviour 3, and
 * `+0xE = SI - 1` - the map cell pointer, stored here as the cell index.
 *
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_b8(uint32_t col, uint32_t row)
{
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u, (uint16_t)(row * 16u +
                                 _obj_spawn_style_nudge(NULL, col, row) - 8u));
    wr_le_u16(r + 4u, 0x00AFu);
    wr_le_u16(r + 6u, 0x0000u);
    wr_le_u16(r + 8u, 0x0000u);
    wr_le_u16(r + 0xAu, 0x0000u);
    wr_le_u16(r + 0xCu, 0x0000u);
    wr_le_u16(r + 0xEu, (uint16_t)(row * OBJ_COLS + col));
    _obj_slot_class[_obj_live_count - 7u] = 3u;
    _obj_live_count++;
}


/* 1000:52A2 (0xAC, frame 0x96 -> behaviour 7), 1000:52C7 (0xAD, frame 0x9C ->
 * behaviour 6) and 1000:52EB (0xAE, frame 0x38 -> behaviour 4): one appended
 * record each, then the shared tail 1000:5639 snaps its y to the terrain step.
 * The campfire (0x96) is behaviour 7 and the torch (0x9C) is 6.
 * 1000:5662 (called first) nudges y by 3 on a style-6 level when the cell
 * directly below is marked in the static 0x1F20 table. */
static uint32_t _obj_spawn_style_nudge(const uint8_t *map, uint32_t col,
                                       uint32_t row)
{
    static const uint8_t nudgeBits[19] = {
        0x00, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x07, 0xE0, 0x00,
        0x02, 0x02, 0x00, 0x03, 0xC3, 0x81, 0x80, 0x01, 0x80,
    };
    uint16_t cell = (uint16_t)(row * OBJ_COLS + col);
    uint8_t tile;

    if (!map) map = level_map();
    if (level_style() != 6u || !map || cell + OBJ_COLS >= OBJ_MAP_CELLS)
        return 0u;
    tile = map[cell + OBJ_COLS];
    if (tile > 0x90u) return 0u;
    return (nudgeBits[tile >> 3] & (uint8_t)(1u << (7u - (tile & 7u)))) ? 3u
                                                                        : 0u;
}


uint32_t object_spawn_style_y_nudge(uint32_t col, uint32_t row)
{ return _obj_spawn_style_nudge(NULL, col, row); }


/**
 * @brief Append the style record's decoration rows.
 *
 * @param map the 640-cell map
 * @param col the column
 * @param row the row
 * @param frame the frame index
 * @param behaviour the behaviour id
 * @param f8 the field-8 value
 * @param fc the field-C value
 * @param fe the field-E value
 */
static void _obj_spawn_append_style(const uint8_t *map, uint32_t col,
                                    uint32_t row, uint32_t frame,
                                    uint32_t behaviour, uint32_t f8,
                                    uint32_t fc, uint32_t fe)
{
    uint8_t *r;
    uint32_t y3, probeCell;
    int32_t bx;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    y3 = _obj_spawn_style_nudge(map, col, row);
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)(col * 16u));
    wr_le_u16(r + 2u, (uint16_t)(row * 16u + y3));
    wr_le_u16(r + 4u, (uint16_t)(frame));
    wr_le_u16(r + 6u, 0u);
    wr_le_u16(r + 8u, (uint16_t)(f8));
    wr_le_u16(r + 0xAu, 0u);
    wr_le_u16(r + 0xCu, (uint16_t)(fc));
    wr_le_u16(r + 0xEu, (uint16_t)(fe));
    _obj_slot_class[_obj_live_count - 7u] = (uint8_t)behaviour;
    _obj_live_count++;
    bx = _obj_core_step_probe_at(r, 8, 0x10, &probeCell); // 1000:5639
    wr_le_u16(r + 2u, (uint16_t)(bx - 0x10));
}


/** @brief 1000:51F9 decode_terrain_row (tile 0xA9): five records from the 0x1EFE
 * offset table at (col*16 + dx, row*16 + 1 + dy), frames 0x33..0x37, `+8 = 0`,
 * behaviour 0, plus one 0x32B9 background entry whose `+4` points at their first
 * record and whose `+8` is the map pointer + 0x20.
 * Its first two instructions mark the two map cells one row below as 0xC2/0xC9 -
 * the ids 1000:DF2C tests, so this handler is what makes the settle's background
 * mark reachable.
 *
 * @param map the 640-cell map
 * @param col the column
 * @param row the row
 */
static void _obj_spawn_append_row9(uint8_t *map, uint32_t col, uint32_t row)
{
    static const uint8_t table[5][3] = {
        /* 0x1EFE: dx, dy, frame */
        { 0x10, 0x13, 0x33 }, { 0x16, 0x15, 0x34 }, { 0x1C, 0x16, 0x35 },
        { 0x23, 0x15, 0x36 }, { 0x29, 0x13, 0x37 },
    };
    uint16_t first = (uint16_t)(_obj_live_count + 1u);

    for (uint16_t i = 0; i < 5u; i++) {
        uint8_t *r;

        if (_obj_live_count + 1u >= OBJ_SLOTS) return;
        r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
        wr_le_u16(r + 0u, (uint16_t)(col * 16u + table[i][0]));
        wr_le_u16(r + 2u, (uint16_t)(row * 16u + 1u + table[i][1]));
        wr_le_u16(r + 4u, table[i][2]);
        wr_le_u16(r + 6u, 0u);
        wr_le_u16(r + 8u, 0u);
        _obj_slot_class[_obj_live_count - 7u] = 0u; // behaviour 0
        _obj_live_count++;
    }
    /* 1000:51F9: mark the two cells one row below as the ids 1000:DF2C looks
     * for.  The handler's SI is one past the matched cell, so the pair lands at
     * (col+1, row+1) and (col+2, row+1) - live level 16 row 17 reads
     * 0x33 0xC2 0xC9 at cols 14..16 with the 0xA9 at col 14 (the 0x33 is the
     * level's own tile, untouched).  A write outside the map is dropped
     * (D-270); no shipped level reaches one. */
    {
        uint32_t cell = (uint32_t)row * OBJ_COLS + col;

        if (map && cell + OBJ_COLS + 1u < OBJ_MAP_CELLS)
            map[cell + OBJ_COLS + 1u] = 0xC2u;
        if (map && cell + OBJ_COLS + 2u < OBJ_MAP_CELLS)
            map[cell + OBJ_COLS + 2u] = 0xC9u;
    }
    if (_obj_bg_count < sizeof _obj_bg / sizeof _obj_bg[0]) {
        uint32_t mcell = (row + 1u) * OBJ_COLS + col + 1u;

        _obj_bg[_obj_bg_count].x = (uint16_t)(col * 16u);
        _obj_bg[_obj_bg_count].y = (uint16_t)(row * 16u + 1u);
        _obj_bg[_obj_bg_count].dst = first;
        _obj_bg[_obj_bg_count].mark =
            mcell < OBJ_MAP_CELLS ? (uint16_t)mcell : 0xFFFFu;
        _obj_bg_count++; // [0x32B9]
    }
}


/** @brief 1000:556B terrain_append_decoration, the tile handler 0xF0/0xF1/0xF2 share
 * (0xF0 is a four-instruction stub that sets BX and jumps in; the frame is
 * 0xC1 / 0xC5 / 0xC9): an appended record
 * {x, y, frame, +6 = 0, +8 = frame, +0xA = 0} with behaviour 1.
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param tile the tile byte
 */
static void _obj_spawn_append_decoration(uint32_t x, uint32_t y, uint8_t tile)
{
    static const uint16_t frame[3] = { 0x00C1u, 0x00C5u, 0x00C9u };
    uint16_t f = frame[tile - 0xF0u];
    uint8_t *r;

    if (_obj_live_count + 1u >= OBJ_SLOTS) return;
    r = _obj_array + (_obj_live_count + 1u) * OBJ_STRIDE;
    wr_le_u16(r + 0u, (uint16_t)x);
    wr_le_u16(r + 2u, (uint16_t)y);
    wr_le_u16(r + 4u, f);
    wr_le_u16(r + 6u, 0u);
    wr_le_u16(r + 8u, f);
    wr_le_u16(r + 0xAu, 0u);
    _obj_slot_class[_obj_live_count - 7u] = 1u; // [0x3E5F + count-8] = 0x01
    _obj_live_count++;
}


/**
 * @brief Spawn every map cell that holds one placed-object tile.
 *
 * @param map the 640-cell map
 * @param tile the tile byte
 */
static void _spawn_terrain_tile(uint8_t *map, uint32_t tile)
{
    for (uint16_t row = 0; row < OBJ_ROWS; row++)
        for (uint16_t col = 0; col < OBJ_COLS; col++) {
            if (map[row * OBJ_COLS + col] != tile) continue;
            if (tile == 0xBBu) _obj_spawn_bb(col, row);
            else if (tile == 0xBEu) _obj_spawn_be(col, row);
            else if (tile == 0xBFu) _obj_spawn_join_bf(col);
            else if (tile == 0xC0u) _obj_spawn_c0(col, row);
            else if (tile == 0xC1u) _obj_spawn_c1(col, row);
            else if (tile == 0xB6u) _obj_spawn_b6(col, row);
            else if (tile == 0xB7u) _obj_spawn_b7(map, col, row);
            else if (tile == 0xB8u) _obj_spawn_b8(col, row);
            else if (tile >= 0xAFu && tile <= 0xB2u)
                _obj_spawn_write_pair(tile, col * 16u, row * 16u);
            else if (tile == 0xA9u) _obj_spawn_append_row9(map, col, row);
            else if (tile == 0xAAu) _obj_spawn_place_human(col, row);
            else if (tile == 0xABu) _obj_spawn_append_spear(col, row);
            else if (tile == 0xACu)
                _obj_spawn_append_style(map, col, row, 0x96u, 7u, 0xFFFFu, 0u,
                                        0u);
            else if (tile == 0xADu)
                _obj_spawn_append_style(map, col, row, 0x9Cu, 6u, 0xFFFFu, 0u,
                                        0u);
            else if (tile == 0xAEu)
                /* 1000:52EB differs from 0x52A2/0x52C7: +8 and +0xA are 0
                 * and BOTH +0xC and +0xE are 0xFFFF (+0xE is the give/hold
                 * gate 0x9CF6 tests with bit 15). */
                _obj_spawn_append_style(map, col, row, 0x38u, 4u, 0x0000u,
                                        0xFFFFu, 0xFFFFu);
            else
                _obj_spawn_append_decoration(col * 16u, row * 16u,
                                             (uint8_t)(tile));
        }
}


uint32_t object_spawn_load_terrain_objects(uint8_t *map)
{
    if (!map) return _obj_live_count;
    _obj_live_count = OBJ_LIVE0; // 1000:516F
    memset(_obj_slot_class + 1u, 0, 20u); // 1000:5175: 0x3E5F..0x3E72
    for (uint16_t t = 0; t < 25u; t++) {
        uint8_t tile = _obj_tiles[t];
        /* all 25 tiles' bodies are ported; the guard is the table walk's
         * tile list. */
        if (tile != 0xA9u && tile != 0xAAu && tile != 0xABu && tile != 0xACu &&
            tile != 0xADu && tile != 0xAEu && tile != 0xAFu && tile != 0xB0u &&
            tile != 0xB1u && tile != 0xB2u && tile != 0xB6u && tile != 0xB7u &&
            tile != 0xB8u && tile != 0xBBu && tile != 0xBEu && tile != 0xBFu &&
            tile != 0xC0u && tile != 0xC1u && (tile < 0xF0u || tile > 0xF2u))
            continue;
        _spawn_terrain_tile(map, tile);
    }
    return _obj_live_count;
}


uint8_t object_spawn_bg_count(void) { return _obj_bg_count; }


void object_spawn_bg_at(uint32_t i, uint32_t *x, uint32_t *y)
{
    if (i >= _obj_bg_count) {
        *x = 0;
        *y = 0;
        return;
    }
    *x = _obj_bg[i].x;
    *y = _obj_bg[i].y;
}


/** @brief 1000:D5E4: the per-frame 0x32B9 pass over the 0xA9 log records.  A record
 * whose +8 the settle's 0xDF2C poked (the tables 0x689E/0x68BE write 0xFF at
 * firstRecord + 8 + 0x12*k) falls: +2 per tick, and past y 0x138 it sets +6 bit
 * 7, which hides it.  The same pass rewrites the two map marker cells from the
 * table at 0x6697, encoding which records are marked.
 *
 * Live-verified: the five records fall in turn (y 276 -> 314, +6 = 0xFF) and
 * the markers go C2/C9 -> C2/CA -> C3/00 -> 00/00, as the table below encodes.
 */
void _obj_spawn_bg_tick(void)
{
    /* DS:0x6697: five 2-byte record entries, then the two marker runs. */
    static const uint8_t tbl[26] = {
        0x04, 0x00, 0x02, 0x00, 0x01, 0x04, 0x00, 0x02, 0x00,
        0x01, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0x00,
        0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0x00,
    };
    uint8_t i;

    for (i = 0; i < _obj_bg_count; i++) {
        uint16_t dst = _obj_bg[i].dst;
        uint8_t *r;
        uint8_t cx = 0, dx = 0, t = 0, k;

        if (dst >= OBJ_SLOTS) continue;
        r = _obj_array + dst * OBJ_STRIDE;
        for (k = 0; k < 5u; k++, r += OBJ_STRIDE, t += 2u) {
            if (!(r[6] & 0x80u)) {
                uint16_t y;

                if (rd_le_u16(r + 8u) == 0u) continue; // 1000:D614: no OR
                y = (uint16_t)((uint32_t)rd_le_u16(r + 2u) + 2u);
                wr_le_u16(r + 2u, y);
                if (y > 0x138u) r[6] = 0xFFu; // 1000:D61F
            }
            cx |= tbl[t];
            dx |= tbl[t + 1u];
        }
        {
            uint16_t mark = _obj_bg[i].mark;
            uint8_t *map = (uint8_t *)level_map();

            /* An edge pair outside the map is dropped (D-270). */
            if (mark != 0xFFFFu && map) {
                if (mark < OBJ_MAP_CELLS)
                    map[mark] = tbl[0x0Au + cx]; // 1000:D638
                if (mark + 1u < OBJ_MAP_CELLS)
                    map[mark + 1u] = tbl[0x12u + dx]; // 1000:D641
            }
        }
    }
}


/* Mark the background pixel under the actor's feet. */
int32_t _obj_spawn_bg_mark(const uint8_t *actor)
{
    uint32_t sub, cell = _obj_core_map_at_abs((int32_t)_obj_foot_x,
                                              (int32_t)_obj_foot_y, &sub);
    uint8_t idx = (uint8_t)((actor - _obj_array) / OBJ_STRIDE);
    const uint8_t *tbl;
    int8_t bl;

    if (cell < 0xC2u || cell > 0xCFu) return 0; // 1000:DF77
    for (uint16_t i = 0; i < _obj_bg_count; i++) {
        uint16_t rx = _obj_bg[i].x, ry = _obj_bg[i].y;
        int32_t d = (int32_t)_obj_foot_x - (int32_t)((rx + 0x10u) & 0xFFF0u);

        if ((_obj_foot_y & 0xFFF0u) != ((ry + 0x10u) & 0xFFF0u)) continue;
        if (d < 0 || d >= 0x20) continue;
        if (idx < 8u) {
            if (rd_le_u16(actor + 8u) > 1u) return -1; // 1000:E001
            /* 1000:E007/E009: type 1 -> 0x68BE, everything <= 1 -> 0x689E */
            tbl = rd_le_u16(actor + 8u) == 1u ? _obj_foot_b : _obj_foot_a;
        } else {
            int32_t side = _obj_side;
            if (side == 0) return -1; // 1000:DFF8
            tbl = side < 0 ? _obj_foot_b : _obj_foot_a;
        }
        /* 1000:DFE2/0xE00C: the table index is d, the offset from the aligned
         * window start (not foot_x - rx, which is d + 0x10). */
        bl = tbl[(uint32_t)d & 0x1Fu];
        if (bl) {
            uint32_t at = (uint32_t)_obj_bg[i].dst * OBJ_STRIDE + (uint32_t)bl;

            if (at < sizeof _obj_array) _obj_array[at] = 0xFFu;
        }
        return -1;
    }
    return 0;
}
