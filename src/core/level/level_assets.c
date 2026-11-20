#include "game_resources.h"
#include "level/level.h"
#include "level/level_internal.h"
#include "object/object.h"
#include "osystem.h"
#include "width.h"
#include "res.h"
#include "byteorder.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- the level's visual assets -----------------------------------------
 *
 * DEFS\BD<style>.BIN: the defs walk - the loader steps the buffer
 * sequentially (array_max_word over the A/B offset tables, then the fixed
 * blocks); field L (the u16 background index) selects BACKS\BG<n>.RAW.
 * LEVELS\BS<style>.BLK: the 169x16x16 tile _atlas behind a 0x400 APL header.
 * BACKS\BG<n>.RAW: 512x320 chunky + a 768-byte 6-bit palette; draw_map_digit
 * copies only its first 64 entries (0xC0 bytes) into the DAC. */
#define WORLD_W 512u
#define WORLD_H 320u

#define ATLAS_TILES 169u

/* The BD walk: returns the end offset (== the file size
 * for all six shipped files), 0 on a bounds failure.  The format's "D" row -
 * the 5 far pointers - is loader-constructed, 0 file bytes; row J advances
 * by the LITERAL si += word[si] (the length word is part of the skip), then
 * 8 bytes; field M (a u16 count + 4-byte entries) is the palette-rotation
 * table, handed back to the caller.  Byte-exact over BD1..BD6. */
typedef struct BdPage {
    const uint8_t *aTable; // field A: the per-tile slope profiles
    uint32_t aSize;
    const uint8_t *items; // field B: 256 word offsets
    uint32_t itemsSize; // maxB + 0x20: the record area
    const uint8_t *jBytes; // field J: the 8 bytes at 0x1E98
    const uint8_t *rowBlock; // field I: 0xC0 B (the 0x80..0xBF bank)
    const uint8_t *nList; // field N: count + 17-byte {id, 16}
    uint8_t nCount;
    const uint8_t *oList; // field O: count + 16 id bytes
    uint8_t oCount;
    const uint8_t *cBlock; // field C: the 0xA9 background sprites
    const uint8_t *eBlock; // field E: the 0x80-byte A9 log sprite
    const uint8_t *hBlock; // field H: the two 0x100-byte sprites
    const uint8_t *gBlock; // field G: 3 layers x n x 0x100 sprites
    uint16_t gCount; // n (blocks per layer)
    const uint8_t *pBlock; // field P: the 0x1000-byte exit art
    uint32_t pSize;
} BdPage;

static uint8_t *_atlas; // 0x400 + 169*256
static uint8_t *_bg_image; // 512*320 + 768
static uint8_t *_world; // the 512x320 composite (BG + terrain)

/* DEFS\BD<style>.BIN's page tables (the terrain-page system behind
 * page_offset_to_ptr, 1000:F123): 256 x 16 mask bytes per tile id and the
 * 16-word item records field B offsets into.  Built by the same loader that
 * `load_terrain_pages` (1000:505C) implements. */
static uint8_t _page_mask[256][16];
static uint8_t _page_opacity[32]; // 0x1FA4: the 32-colour opacity flags
/* BD field I, copied by copy_level_row (0x4EE9) into the level palette's
 * 0x80..0xBF bank: the level background's 64-colour gradient. */
static uint8_t _bd_row_block[0xC0];
static uint8_t _bd_c_block[0x600]; // field C: the 0xA9 background art
static uint8_t _bd_g_block[3u * 4u * 0x100u]; // field G: 3 layers x 4 max
static uint8_t _bd_e_block[0x80]; // field E: the A9 log sprite
static uint8_t _bd_h_block[0x200]; // field H: frames 0x26/0x27
static uint8_t _slope_profiles[0x400]; // field A: the slope profiles
static uint32_t _slope_size;
static uint16_t _bd_anim_frames; // field F -> [0x6532]
static uint8_t _bd_c_have;
static uint8_t _bd_p_block[0x1000]; // field P: the exit/igloo art (0xB9)
static uint8_t _bd_p_have;
#if defined(HUMANSRE_TEST_PROBES)
static uint16_t _level_bg_index; // the BD page's field L (test probe)
#endif
static uint8_t *_page_items; // field B + its record area
static uint32_t _page_items_size;
static uint8_t _page_valid;
static uint8_t _k_bytes[4]; // field K: [0x6F20], [0x6F0B]
/* Field M, the palette-rotation table: one 4-byte entry per range
 * {timer, reload, first DAC entry, entry count}; palette_rotate_table
 * (1000:8C78) walks it from 202F:0x1FC6.  Only BD3 has a range. */
static uint8_t *_anim_table;
static uint16_t _anim_count;

/**
 * @brief Release the tile atlas once the level's palette is built.
 */
void level_assets_release_atlas(void)
{
    g_osys->memFree(_atlas);
    _atlas = NULL;
}

/**
 * @brief Release the loaded level's visual banks.
 */
static void _visuals_free(void)
{
    level_assets_release_atlas();
    g_osys->memFree(_bg_image);
    _bg_image = NULL;
    g_osys->memFree(_world);
    _world = NULL;
    g_osys->memFree(_anim_table);
    _anim_table = NULL;
    g_osys->memFree(_page_items);
    _page_items = NULL;
    _page_items_size = 0;
    _page_valid = 0;
    _anim_count = 0;
    memset(_page_opacity, 0, sizeof _page_opacity);
}

/**
 * @brief Walk the BD record's streams and fill the level's terrain blocks.
 *
 * @param b the BD record bytes
 * @param size the size
 * @param bgIndex the background index
 * @param anim the animation cursor
 * @param animEntries the animation entry count
 * @param page the page buffer
 * @return the end offset, or 0 on a short record
 */
static uint32_t _bd_walk(const uint8_t *b, uint32_t size, uint16_t *bgIndex,
                         const uint8_t **anim, uint16_t *animEntries,
                         BdPage *page)
{
    uint32_t si = 0;

    /* A / B: 256-word offset tables whose largest entry is the record's
     * size sentinel; the strides 0x10 / 0x20 pad up afterwards. */
    for (uint16_t pass = 0; pass < 2u; pass++) {
        if (si + 512u > size) return 0;
        uint16_t m = 0;
        for (uint16_t i = 0; i < 256u; i++) {
            uint16_t v = rd_le_u16(b + si + i * 2u);
            if (v > m) m = v;
        }
        if (pass == 0u) { // A: the per-tile slope profiles
            page->aTable = b + si;
            page->aSize = m;
        }
        if (pass == 1u) { // B: the item offset table
            page->items = b + si;
            page->itemsSize = m + 0x20u;
        }
        si += m + (pass ? 0x20u : 0x10u);
    }

    page->cBlock = b + si; // C: the 0xA9 background art
    si += 0x600u;
    page->eBlock = b + si; // E: the A9 log sprite (D points here)
    si += 0x80u;
    if (si + 2u > size) return 0;
    uint16_t n = rd_le_u16(b + si);
    page->gBlock = b + si + 2u;
    page->gCount = n; // F: n; G: 3 layers x n blocks
    si += 2u + 3u * n * 0x100u;
    if (si + 0x200u + 0xC0u > size) return 0;
    page->hBlock = b + si; // H: two 0x100 sprites (frames 0x26/0x27)
    page->rowBlock = b + si + 0x200u; // I: copy_level_row's source
    si += 0x200u + 0xC0u; // H + I
    if (si + 2u > size) return 0;
    {
        uint16_t skip = rd_le_u16(b + si);
        if (si + skip + 8u > size) return 0;
        page->jBytes = b + si + skip; // J: the 8 bytes at 0x1E98
        si += skip + 8u;
    }
    if (si + 4u > size) return 0;
    _k_bytes[0] = b[si];
    _k_bytes[1] = b[si + 1]; // K: [0x6F20] = two tile ids
    _k_bytes[2] = b[si + 2];
    _k_bytes[3] = b[si + 3]; // K2: [0x6F0B]
    si += 4u;
    if (si + 2u > size) return 0;
    *bgIndex = rd_le_u16(b + si); // L
    si += 2u;
    if (si + 2u > size) return 0;
    uint16_t m = rd_le_u16(b + si);
    if (si + 2u + 4u * m > size) return 0;
    *anim = b + si + 2u; // M: the rotation table
    *animEntries = m;
    si += 2u + 4u * m;
    if (si + 1u > size) return 0;
    n = b[si];
    page->nList = b + si;
    page->nCount = (uint8_t)(n);
    si += 1u + 17u * n; // N
    if (si + 1u > size) return 0;
    n = b[si];
    page->oList = b + si;
    page->oCount = (uint8_t)(n);
    si += 1u + n; // O
    if (si + 0x1000u > size) return 0;
    page->pBlock = b + si; // P: the 0xB9 exit/igloo art
    page->pSize = 0x1000u;
    si += 0x1000u;
    if (si + 2u > size) return 0;
    return si + 2u; // Q
}

/**
 * @brief 1000:5137: expand four mask bytes into 32 opacity flags, bit 0 first.
 *
 * @param dst the 32-byte destination
 * @param four the four-pixel value
 */
static void _mask_expand(uint8_t *dst, const uint8_t *four)
{
    uint8_t bit = 0;

    memset(dst, 0, 32u);
    for (uint16_t b = 0; b < 4u; b++)
        for (uint16_t k = 0; k < 8u; k++)
            dst[bit++] = (uint8_t)((four[b] >> k) & 1u ? 0xFFu : 0u);
}

/**
 * @brief 1000:5137 into the draw pass's scratch (`_page_opacity`).
 *
 * @param four the four-pixel value
 */
static void _page_mask_build(const uint8_t *four)
{ _mask_expand(_page_opacity, four); }

/**
 * @brief The first opaque row of one 16-px tile column (0xFF when empty).
 *
 * @param tile the tile byte
 * @param col the column
 * @return the first opaque row, or 0xFF when empty
 */
static uint8_t _page_column_top(const uint8_t *tile, uint32_t col)
{
    for (uint16_t row = 0; row < 16u; row++)
        if (_page_opacity[tile[row * 16u + col]]) return (uint8_t)row;
    return 0xFFu;
}

/**
 * @brief draw_terrain2 (1000:505C): build the 256x16 table at 0x92A0 from the
 * tile _atlas, plus the per-tile column masks from the page's J and N blocks.
 *
 * `page->jBytes + 4` is the second four-byte opacity mask (the first four
 * drive draw_objects' item records, 1000:558B).  ids > 0xA8 and the field-O
 * ids are wholly solid; every other column holds the row of its first opaque
 * pixel (0xFF when the column is empty); the field-N records then overwrite
 * the 16 bytes.
 *
 * @param page the page buffer
 */
static void _page_table_build(const BdPage *page)
{
    memset(_page_mask, 0xFF, sizeof _page_mask);
    if (!_atlas || !page->jBytes) return;
    _page_mask_build(page->jBytes + 4u);
    for (uint16_t id = 0; id <= 0xA8u; id++) {
        uint8_t blocked = 0;
        const uint8_t *tile;
        uint8_t col;

        for (uint16_t k = 0; k < page->oCount; k++)
            if (page->oList[1u + k] == id) {
                blocked = 1;
                break;
            }
        if (blocked) continue;
        for (col = 0; col < 16u; col++) {
            tile = _atlas + 0x400u + id * 0x100u;
            _page_mask[id][col] = _page_column_top(tile, col);
        }
    }
    for (uint16_t k = 0; k < page->nCount; k++) {
        const uint8_t *e = page->nList + 1u + 17u * k;
        memcpy(_page_mask[e[0]], e + 1u, 16u);
    }
}

/**
 * @brief Build one 16-bit item row from the atlas cursor and advance it.
 *
 * @param si the atlas cursor (in/out)
 * @return the 16-bit item row
 */
static uint16_t _item_row_word(uint16_t *si)
{
    uint16_t word = 0;

    for (uint16_t col = 0; col < 16u; col++) { // 1000:55D9
        uint8_t px = *si < 0x400u + ATLAS_TILES * 0x100u ? _atlas[*si] : 0u;

        (*si)++;
        if (px < 32u && _page_opacity[px]) word |= 0x8000u >> col;
    }
    return word;
}

/** @brief draw_objects (1000:558B): field B's item records are the terrain's per-row
 * collision masks, NOT stored - the shipped descriptors are all zero and this
 * pass fills them from the tile art.  `1000:5137` builds the `0x1FA4` flags
 * from the J block's FIRST four bytes (0x1E98; the page table above uses the
 * second four), then the pass walks the 0xA9 offset words:
 *
 *   55AF  a negative offset ends the walk;
 *   55B7  a word equal to tab[0] is skipped (the shared "no collision" record)
 *         and the art cursor SI advances one tile;
 *   55C8  any word already non-zero is left alone - and that path does NOT
 *         advance SI, so a tile whose descriptor an earlier tile already
 *         computed reuses whatever art SI sits on.  Kept as the listing has it
 *         (the live table is the check);
 *   55DB  a zero word is 16 pixels of `ES:SI`, bit 0x8000 >> col for every
 *         opaque one, and SI advances 16.
 *
 * @param page the page buffer
 */
static void _item_masks_build(const BdPage *page)
{
    uint16_t si = 0x400u; // 1000:5595: ES:SI, the tiles
    uint16_t tab0;

    if (!_page_items || !_atlas || !page->jBytes || !_page_valid) return;
    _page_mask_build(page->jBytes); // 0x1FA4 from 0x1E98's first 4
    tab0 = rd_le_u16(_page_items);
    for (uint16_t id = 0; id <= 0xA8u; id++) {
        uint16_t off = rd_le_u16(_page_items + 2u * id);

        if (off & 0x8000u) break; // 1000:55AF JS
        if (off == tab0) { // 1000:55B7
            si += 0x100u;
            continue;
        }
        if ((uint32_t)off + 32u > _page_items_size) continue;
        for (uint16_t row = 0; row < 16u; row++) {
            uint16_t o = (uint16_t)(off + 2u * row);

            if (rd_le_u16(_page_items + o) != 0u) continue; // 1000:55CC
            wr_le_u16(_page_items + o, _item_row_word(&si));
        }
    }
}

/** @brief draw_map_digit (1000:4F22) + the BD walk: the style's defs, _atlas and
 * background, all off the current disc (everything ships on HUMANS.001).
 *
 * @param style the level style
 * @return 0 on success, -1 when an asset is missing
 */
int32_t _level_assets_load(uint32_t style)
{
    _visuals_free();

    char name[32];
    snprintf(name, sizeof name, game_resource_name(GAME_RES_DEFS_BD_FMT),
             style);
    uint32_t bdSize = 0;
    uint16_t chk = 0;
    uint8_t *bd = res_load_chunk(name, &bdSize, &chk);
    if (!bd) return -1;
    uint16_t bgIdx = 0;
    uint16_t animN = 0;
    const uint8_t *anim = NULL;
    BdPage page;
    memset(&page, 0, sizeof page);
    uint32_t end = _bd_walk(bd, bdSize, &bgIdx, &anim, &animN, &page);
    memset(_bd_row_block, 0, sizeof _bd_row_block);
    if (end == bdSize && page.rowBlock)
        memcpy(_bd_row_block, page.rowBlock, sizeof _bd_row_block); // 0x4EE9
    /* 1000:4D58's add_delta_nonzero(0x20) applies to E and H too. */
    memset(_bd_e_block, 0, sizeof _bd_e_block);
    memset(_bd_h_block, 0, sizeof _bd_h_block);
    if (end == bdSize && page.eBlock && page.hBlock) {
        for (uint16_t i = 0; i < sizeof _bd_e_block; i++)
            _bd_e_block[i] =
                page.eBlock[i] ? (uint8_t)(page.eBlock[i] + 0x20u) : 0u;
        for (uint16_t i = 0; i < sizeof _bd_h_block; i++)
            _bd_h_block[i] =
                page.hBlock[i] ? (uint8_t)(page.hBlock[i] + 0x20u) : 0u;
        object_render_publish_deh_sprites(_bd_e_block, _bd_h_block);
    }
    memset(_bd_g_block, 0, sizeof _bd_g_block);
    if (end == bdSize && page.gBlock && page.gCount && page.gCount <= 4u) {
        uint16_t blocks = u16(3u * page.gCount);

        /* 1000:4D58's add_delta_nonzero(0x20) applies to every G block. */
        for (uint16_t i = 0; i < blocks * 0x100u; i++) {
            uint8_t v = page.gBlock[i];
            _bd_g_block[i] = v ? (uint8_t)(v + 0x20u) : 0u;
        }
        object_render_publish_g_sprites(_bd_g_block, blocks,
                                        (uint8_t)(page.gCount));
    }
    _bd_anim_frames = (end == bdSize) ? page.gCount : 0u;
    /* field A (the per-tile slope profiles) is read by behaviour 4's
     * projectile; copy it before `bd` is freed. */
    memset(_slope_profiles, 0, sizeof _slope_profiles);
    _slope_size = 0;
    if (end == bdSize && page.aTable && page.aSize &&
        page.aSize <= sizeof _slope_profiles) {
        memcpy(_slope_profiles, page.aTable, page.aSize);
        _slope_size = page.aSize;
    }
    memset(_bd_c_block, 0, sizeof _bd_c_block);
    _bd_c_have = 0;
    if (end == bdSize && page.cBlock) {
        /* 1000:4D58-0x4D68: level_setup_screen's add_delta_nonzero(0x600,
         * 0x20) moves the C block into the 0x20..0x3F art palette. */
        for (uint16_t i = 0; i < sizeof _bd_c_block; i++) {
            uint8_t v = page.cBlock[i];
            _bd_c_block[i] = v ? (uint8_t)(v + 0x20u) : 0u;
        }
        _bd_c_have = 1;
    }
    memset(_bd_p_block, 0, sizeof _bd_p_block);
    _bd_p_have = 0;
    if (end == bdSize && page.pBlock && page.pSize >= sizeof _bd_p_block) {
        memcpy(_bd_p_block, page.pBlock, sizeof _bd_p_block); // 0x54A9
        _bd_p_have = 1;
    }
    if (end == bdSize && animN) {
        _anim_table = g_osys->memAlloc(4u * animN);
        if (_anim_table) {
            memcpy(_anim_table, anim, 4u * animN);
            _anim_count = animN;
        }
    }
    /* The terrain page table (draw_terrain2 1000:505C) is built from the
     * tile _atlas once that has loaded - _page_table_build below. */
    if (page.items && page.itemsSize <= bdSize) {
        g_osys->memFree(_page_items);
        _page_items = g_osys->memAlloc(page.itemsSize);
        if (_page_items) {
            memcpy(_page_items, page.items, page.itemsSize);
            _page_items_size = page.itemsSize;
            _page_valid = 1;
        }
    }
    if (end != bdSize) {
        g_osys->memFree(bd);
        return -1;
    }

    snprintf(name, sizeof name, game_resource_name(GAME_RES_LEVELS_BS_FMT),
             style);
    _atlas = res_load_chunk(name, &bdSize, &chk);
    if (!_atlas || bdSize != 0x400u + ATLAS_TILES * 0x100u) {
        g_osys->memFree(bd);
        _visuals_free();
        return -1;
    }
    /* the page table is built from the _atlas while the BD image (J/O/N)
     * is still alive; the pointers in `page` point into it. */
    _page_table_build(&page);
    /* draw_objects (1000:558B) then fills field B's item records; it needs
     * the same J bytes, so it runs before the BD image is released. */
    _item_masks_build(&page);
    g_osys->memFree(bd);

#if defined(HUMANSRE_TEST_PROBES)
    _level_bg_index = bgIdx;
#endif
    if (bgIdx != 0u) {
        snprintf(name, sizeof name, game_resource_name(GAME_RES_BACKS_BG_FMT),
                 bgIdx);
        _bg_image = res_load_chunk(name, &bdSize, &chk);
        if (!_bg_image || bdSize != WORLD_W * WORLD_H + 768u) {
            _visuals_free();
            return -1;
        }
        /* draw_map_digit 0x4F3C -> 0x1593 puts this file's palette at the
         * level palette's 0x40..0x7F (the art), and load_level_assets'
         * 0x4BE2 transform rewrites the image: v (0..0x3F) becomes 0x40+v,
         * and the 0xFF empty fill becomes the BD row block's 64-colour
         * gradient: load_level_assets' 0x4BE2 loop walks the image in 64
         * chunks of 0xA00 bytes and uses 0x80 + the chunk as the fill index,
         * so the entry steps every 5 rows of the 512-wide image - exactly
         * one ramp over the 320-row _world (0x80 + i / 0xA00). */
        for (uint32_t i = 0; i < WORLD_W * WORLD_H; i++) {
            uint8_t v = _bg_image[i];
            if (v == 0xFFu)
                _bg_image[i] = (uint8_t)(0x80u + (i / 0xA00u) % 64u);
            else _bg_image[i] = (uint8_t)(v < 0x40u ? 0x40u + v : v);
        }
    }

    _world = g_osys->memAlloc(WORLD_W * WORLD_H);
    if (!_world) {
        _visuals_free();
        return -1;
    }
    return 0;
}

/**
 * @brief Stamp one 64x64 P-block into the background at (x, y).
 *
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _stamp_p_block(int32_t x, int32_t y)
{
    for (uint16_t r = 0; r < 64u; r++)
        for (uint16_t c = 0; c < 64u; c++) {
            uint8_t v = _bd_p_block[r * 64u + c];
            int32_t px = x + (int32_t)c, py = y + (int32_t)r;

            if (!v || px < 0 || py < 0 || px >= (int32_t)WORLD_W ||
                py >= (int32_t)WORLD_H)
                continue;
            /* level_setup_screen's add_delta_nonzero(0x1000, 0x20) pass
             * moves the P block into the 0x20..0x3F art palette. */
            _bg_image[py * (int32_t)WORLD_W + px] = (uint8_t)(v + 0x20u);
        }
}

int32_t level_exit_stamp_y(uint32_t col, uint32_t row)
{ return (int32_t)(row * 16u + object_spawn_style_y_nudge(col, row)) - 0x30; }

/** @brief 1000:5159 load_terrain_a + 1000:54A9: every map cell 0xB9 stamps the BD
 * page's P block (64 x 64, zero transparent) into the background image at
 * (col*16, level_exit_stamp_y(col, row)) - the level's exit/igloo art.
 */
void _level_stamp_exits(void)
{
    if (!_bg_image || !_bd_p_have) return;
    for (uint16_t row = 0; row < MAP_ROWS; row++)
        for (uint16_t col = 0; col < MAP_COLS; col++) {
            if (_level_record()->map[row * MAP_COLS + col] != 0xB9u) continue;
            _stamp_p_block((int32_t)(col * 16u), level_exit_stamp_y(col, row));
        }
}

void level_draw_bg_sprites(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    if (!_bd_c_have) return;
    for (uint16_t i = 0; i < object_spawn_bg_count(); i++) {
        uint32_t x, y;

        object_spawn_bg_at(i, &x, &y);
        object_render_blit_sprite(page, _bd_c_block, 24u, 32u,
                                  (int32_t)x - scrollX, (int32_t)y - scrollY);
        object_render_blit_sprite(page, _bd_c_block + 0x300u, 24u, 32u,
                                  (int32_t)x + 0x20 - scrollX,
                                  (int32_t)y - scrollY);
    }
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
const uint8_t *level_bg_for_probe(void) { return _bg_image; }

uint8_t level_atlas_live_for_probe(void) { return _atlas != NULL; }

#endif /* HUMANSRE_TEST_PROBES */

uint32_t level_anim_frames(void) { return _bd_anim_frames; }

int32_t level_slope_byte(uint32_t id, uint32_t sub)
{
    uint16_t off;

    if (!_slope_size || id > 0xFFu || sub > 0xFu) return 0;
    off = rd_le_u16(_slope_profiles + 2u * id);
    if (off + sub >= _slope_size) return 0;
    return (int8_t)_slope_profiles[off + sub];
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
const uint8_t *level_p_block_for_probe(void)
{ return _bd_p_have ? _bd_p_block : NULL; }

const uint8_t *level_c_block_for_probe(void)
{ return _bd_c_have ? _bd_c_block : NULL; }

uint16_t level_bg_index_for_probe(void) { return _level_bg_index; }
#endif /* HUMANSRE_TEST_PROBES */

uint8_t level_page_free(uint32_t id)
{
    uint32_t off, k;
    uint16_t sum = 0;

    if (!_page_valid || id > 0xFFu) return 0;
    for (k = 0; k < 16u; k++)
        if (!(_page_mask[id][k] & 0x80u)) return 0; // a byte >= 0
    off = rd_le_u16(_page_items + 2u * id); // word[B + 2*id]
    if (off + 32u > _page_items_size) return 0;
    for (k = 0; k < 16u; k++)
        sum = (uint16_t)(sum + rd_le_u16(_page_items + off + 2u * k));
    return sum == 0u;
}

uint32_t level_page_byte(uint32_t id, uint32_t subcol)
{
    if (!_page_valid || id > 0xFFu || subcol > 0xFu) return 0x80u;
    return _page_mask[id][subcol];
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
/* The `[0x1FD6]` item record's `row` word: `B + word[B + 2*id]` then `2*row`,
 * i.e. the 16-bit sub-cell occupancy mask 1000:F25F tests. 0xFFFF when absent. */
uint32_t level_item_byte(uint32_t id, uint32_t off)
{
    uint16_t o;

    if (!_page_valid || id > 0xFFu) return 0x80u;
    o = rd_le_u16(_page_items + 2u * id);
    if (o + off >= _page_items_size) return 0x80u;
    return _page_items[o + off];
}
#endif /* HUMANSRE_TEST_PROBES */

uint32_t level_item_word(uint32_t id, uint32_t row)
{
    uint16_t off;

    if (!_page_valid || id > 0xFFu || row > 0xFu) return 0xFFFFu;
    off = rd_le_u16(_page_items + 2u * id);
    if (off + 32u > _page_items_size) return 0xFFFFu;
    return rd_le_u16(_page_items + off + 2u * row);
}

uint32_t level_special_tile(uint32_t i)
{ return i < 4u ? _k_bytes[i] : 0xFFFFu; }

/**
 * @brief Stamp one 16x16 terrain tile into the world page.
 *
 * @param dst the destination buffer
 * @param src the source buffer
 */
static void _stamp_tile(uint8_t *dst, const uint8_t *src)
{
    for (uint16_t y = 0; y < 16u; y++) {
        for (uint16_t x = 0; x < 16u; x++) {
            uint8_t v = src[y * 16u + x];
            /* draw_terrain 0x502d: a zero tile byte is NOT stored - the
             * background (BG art / 0x80 sky) stays. */
            if (v) dst[y * WORLD_W + x] = (uint8_t)(v + 0x20u);
        }
    }
}

/**
 * @brief Build the static 512x320 world composite (BG + terrain) once per
 * level load: `draw_terrain`'s atlas stamp (`1000:502D`) over `_bg_image`,
 * after load_terrain_a's 0xB9 exit stamps.  The play loop reuses it; its
 * per-frame work is `blit_bitmap_planes` (`1000:4C4E`) over this buffer.
 */
void _level_world_build(void)
{
    if (!_world) return;
    if (_bg_image) memcpy(_world, _bg_image, WORLD_W * WORLD_H);
    else memset(_world, 0, WORLD_W * WORLD_H);

    if (!_atlas) return;
    for (uint16_t r = 0; r < MAP_ROWS; r++) {
        for (uint16_t c = 0; c < MAP_COLS; c++) {
            uint8_t tile = _level_record()->map[r * MAP_COLS + c];

            if (tile >= 0xA9u) continue;
            _stamp_tile(_world + (r * 16u) * WORLD_W + c * 16u,
                        _atlas + 0x400u + tile * 0x100u);
        }
    }
}

/** @brief The tile atlas' palette slice (LEVELS\BS<style>.BLK+0x18). */
const uint8_t *_level_atlas_palette(void)
{ return _atlas ? _atlas + 0x18u : NULL; }

/** @brief The background image's own 64-entry palette. */
const uint8_t *_level_bg_palette(void)
{ return _bg_image ? _bg_image + WORLD_W * WORLD_H : NULL; }

/** @brief The BD row block (field I) that fills palette entries 0x80..0xBF. */
const uint8_t *_level_row_block(void) { return _bd_row_block; }

/** @brief Field M's palette-rotation ranges and their entry count.
 *
 * @param count the entry count out-parameter
 * @return the 4-byte entry table, or NULL when the style has none
 */
uint8_t *_level_anim_ranges(uint16_t *count)
{
    if (count) *count = _anim_count;
    return _anim_table;
}

/** @brief The static 512x320 world composite (BG + terrain). */
const uint8_t *_level_world(void) { return _world; }
