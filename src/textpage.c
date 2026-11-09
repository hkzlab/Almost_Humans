/* textpage.c — the in-level text page: the dialog box + its lines
 * (re_map.md §"The in-level pause menu").  This is the renderer behind the
 * pause menu, the SAVE GAME grid, the out-of-time dialog and the level's own
 * messages ("DISCOVER THE SPEAR!", "WELL DONE!", "YOUR TRIBE IS EXTINCT!!").
 *
 * Ported from the listing:
 *   draw_scroll_text (1000:A08E)  the measure: [0x5BB2] = the max line
 *                                 advance/16 + 2 (the width in 16-px tiles),
 *                                 [0x5BB4] = the line count.
 *   draw_map_object  (1000:9EA3)  the box at page offset 88*(76-(lines+2)*8)
 *                                 + (160-(w+2)*8)/4 + scroll/4 + 88*[0x2]
 *                                 -> [0x5E42]; (w+2) x (lines+2) 16-px tiles.
 *   blit_sprite_planar (A10E)     each box tile: 4 planes, 16 rows x 4 bytes,
 *                                 a 0x54 plane stride, source APL\BOX.SPR
 *                                 (the 9 tiles at 0x000..0x800; loaded by
 *                                 load_level_assets 1000:3468 into [0x13B1]).
 *                                 The tile's pixel (k*4 + p, r) = the byte
 *                                 tile[r*16 + p + 4*k].
 *   blit_text_tile    (1000:47D5) the lines: 16-row pitch, the line's top =
 *                                 box top + 15, the text centred at x=160
 *                                 (draw_text 1000:4989).
 *   text_page_outline (1000:9FB1, its call at 1000:9FA7) the black outline:
 *                                 the box interior, every 4-neighbour of a
 *                                 pixel in [0xF4,0xFC) that is < 0xF4 -> 0.
 *   A203 + A266                   the option markers: the 8x11 patterns at
 *                                 202F:0x5D33 (left) / 0x5D8B (right) /
 *                                 0x5DE3 (blank), placed 16 px inside the
 *                                 box's left edge and 8 px inside its right
 *                                 edge, at boxTop+51+16*row; the grid puts the
 *                                 same pair around the selected cell.
 *
 * The palette: the tile bytes are indices into the level's palette (0..63)
 * and the marker indices 0xE3/0xE4 are the bar's bank (0xC0..0xFF,
 * level_video_init's panel4 copy).  The level DAC base is the working palette
 * 202F:0x14D3 (level.c; re_map.md "The level's working palette") and the
 * box's indices ride it.
 *
 * The text's shading: blit_text_tile writes through write mode 2 with the OR
 * ALU (GC3 := 0x10, 1000:9F76) and the bit mask = its DL argument (0x10 at
 * 1000:9F90), so a glyph pixel becomes `box pixel | 0x10`.  BOX.SPR's fill
 * tile (tile 4) is a vertical ramp EB EA E9 E8 E7 E6 E5 E4 E4 E5 E6 E7 E8 E9
 * EA EB — eight distinct indices that are all (30,0,0) in the palette, so the
 * box looks flat while `| 0x10` turns its rows into the bar bank's grey ramp
 * F4..FB.  The goal page's one line lands on fill rows 3..12 (box top =
 * 76-(lines+2)*8 = 52, text top = +15, glyph rows 4..13) and so prints
 * F8 F7 F6 F5 F4 F4 F5 F6 F7 F8 — the live capture's ramp, exactly.
 */
#include "textpage.h"
#include "font.h"

#include <string.h>

#define TEXT_LIMIT 96u

static const uint8_t *_tp_box;          /* APL\BOX.SPR: 9 x 16x16 tiles */
static uint8_t _tp_arena[16000];

/* 202F:0x5D33 / 0x5D8B / 0x5DE3 — the 8x11 marker patterns (88 bytes). */
static const uint8_t _marker_l[88] = {
    0xE4, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3,
    0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4,
    0xE4, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4,
    0xE3, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3,
    0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4,
    0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3,
    0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3, 0xE4, 0xE4,
    0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4,
};
static const uint8_t _marker_r[88] = {
    0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4,
    0xE4, 0xE4, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3,
    0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4,
    0xE4, 0xE3, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3,
    0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE3, 0xE3,
    0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4,
    0xE4, 0xE4, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4,
    0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE3, 0xE4,
};

void textpage_publish_box(const uint8_t *bank) { _tp_box = bank; }

/**
 * @brief One line's measure: the advance sum (draw_text's own walk).
 *
 * @param p the text cursor (in/out)
 * @param end the end pointer
 * @return the line width
 */
static uint32_t _line_advance(const char *p, const char **end)
{
    char buf[64];
    uint32_t n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\0' && n + 1 < sizeof buf) {
        buf[n] = p[n];
        n++;
    }
    buf[n] = 0;
    *end = p + n;
    return font_text_width(buf);
}

/** @brief draw_scroll_text (1000:A08E): the max line advance -> [0x5BB2] (tiles),
 * the line count -> [0x5BB4].  Lines are CR-separated, NUL-terminated.
 *
 * @param text the text
 * @param tiles the tile count
 * @param lines the line buffer
 */
static void _scroll_text_measure(const char *text, uint32_t *tiles,
                                 uint32_t *lines)
{
    uint32_t maxAdv = 1, nlines = 0;
    const char *p = text;
    while (1) {
        const char *end;
        uint32_t w = _line_advance(p, &end);
        if (w > maxAdv) maxAdv = w;
        nlines++;
        if (*end == 0) break;
        p = end + 1;                    /* past the CR */
        if (*p == 0) {
            nlines++;
            break;
        }
    }
    *tiles = maxAdv / 16u + 2u;
    *lines = nlines;
}

/**
 * @brief Write the four glyph pixels of one arena byte.
 *
 * @param page the page buffer
 * @param y the y coordinate
 * @param x0 the left x
 * @param b the glyph byte
 * @param colour the colour
 * @param mask the mask
 */
static void _glyph_px(uint8_t *page, uint32_t y, uint32_t x0, uint8_t b,
                      uint32_t colour, int32_t mask)
{
    for (uint32_t p = 0; p < 4u; p++)
        if (b & (1u << p)) {
            uint32_t x = x0 + p;

            if (x < 320u) {
                uint32_t at = y * 320u + x;

                if (mask) page[at] |= (uint8_t)colour;
                else page[at] = (uint8_t)colour;
            }
        }
}

/** @brief blit_text_tile (1000:47D5) writes through write mode 2 with the OR ALU
 * (GC3 := 0x10 at 1000:9F76) and the bit mask = DL, so a glyph pixel is
 * `destination | mask`, never a solid colour: the shading comes from
 * whatever the box tile already put there.  `mask` != 0 selects that path;
 * `mask` == 0 writes the solid `colour` for the pages drawn outside a box.
 *
 * @param page the page buffer
 * @param line the text line
 * @param centreX the centring x
 * @param cell the map cell
 * @param colour the colour
 * @param mask the mask
 */
static void _draw_line(uint8_t *page, const char *line, uint32_t centreX,
                       uint32_t cell, uint32_t colour, int32_t mask)
{
    memset(_tp_arena, 0, sizeof _tp_arena);
    font_draw_text(_tp_arena, line, centreX, 0);
    for (uint32_t r = 4u; r < 14u; r++) {
        uint32_t y = cell + r;
        if (y >= 200u) break;
        for (uint32_t c = 0; c < 80u; c++) {
            uint8_t b = _tp_arena[r * 80u + c];

            if (b) _glyph_px(page, y, c * 4u, b, colour, mask);
        }
    }
}

void textpage_draw_line(uint8_t *page, const char *line, uint32_t row,
                        uint8_t colour)
{ _draw_line(page, line, 0xA0u, row, colour, 0); }

void textpage_draw_line_at(uint8_t *page, const char *line, uint32_t centreX,
                           uint32_t row, uint8_t colour)
{ _draw_line(page, line, centreX, row, colour, 0); }

void textpage_draw_line_mask(uint8_t *page, const char *line, uint32_t row,
                             uint8_t mask)
{ _draw_line(page, line, 0xA0u, row, mask, 1); }

void textpage_draw_line_mask_at(uint8_t *page, const char *line,
                                uint32_t centreX, uint32_t row, uint8_t mask)
{ _draw_line(page, line, centreX, row, mask, 1); }

/**
 * @brief Write one 16-pixel tile row (four 4-px groups per byte).
 *
 * @param page the page buffer
 * @param y the y coordinate
 * @param x0 the left x
 * @param row the row
 */
static void _blit_16px(uint8_t *page, uint32_t y, uint32_t x0,
                       const uint8_t *row)
{
    for (uint32_t k = 0; k < 4u; k++)
        for (uint32_t p = 0; p < 4u; p++) {
            uint32_t x = x0 + k * 4u + p;

            if (x < 320u) page[y * 320u + x] = row[p + 4u * k];
        }
}

/**
 * @brief One 16x16 box tile (blit_sprite_planar A10E's layout).
 *
 * @param page the page buffer
 * @param x0 the left x
 * @param y0 the top y
 * @param tile the tile byte
 */
static void _blit_box_tile(uint8_t *page, uint32_t x0, uint32_t y0,
                           const uint8_t *tile)
{
    for (uint32_t r = 0; r < 16u; r++) {
        uint32_t y = y0 + r;
        if (y >= 200u) continue;
        _blit_16px(page, y, x0, tile + r * 16u);
    }
}

/**
 * @brief Write four consecutive pixels from one tile byte group.
 *
 * @param page the page buffer
 * @param y the y coordinate
 * @param x0 the left x
 * @param src the source buffer
 */
static void _blit_4px(uint8_t *page, uint32_t y, uint32_t x0,
                      const uint8_t *src)
{
    for (uint32_t p = 0; p < 4u; p++)
        if (x0 + p < 320u) page[y * 320u + x0 + p] = src[p];
}

/**
 * @brief The marker pair (A203/A266): 8x11, colour bytes as shipped.
 *
 * @param page the page buffer
 * @param x0 the left x
 * @param y0 the top y
 * @param pat the pattern
 */
static void _blit_marker(uint8_t *page, uint32_t x0, uint32_t y0,
                         const uint8_t *pat)
{
    for (uint32_t r = 0; r < 11u; r++) {
        uint32_t y = y0 + r;
        if (y >= 200u) continue;
        for (uint32_t k = 0; k < 2u; k++)
            _blit_4px(page, y, x0 + k * 4u, pat + r * 8u + 4u * k);
    }
}

/** @brief text_page_outline (1000:9FB1, called at the tail of draw_map_object 9EA3):
 * a glyph pixel is any byte in [0xF4,0xFC) — the bar bank's grey ramp; every
 * 4-neighbour that is not one of those (< 0xF4) is set to 0, which is the
 * black outline the original draws around the box text.  The original walks
 * the four planes with the map mask/read map (1000:A012 + A025) and re-tests
 * per plane; with one byte per pixel the same rule is a single raster pass.
 * The scanned rectangle is [0x5D2F] = the box's interior base (box top + 16),
 * lines*16 rows, from the box's left edge for (tiles+1)*16 px (1000:9FBB).
 *
 * @param page the page buffer
 * @param top the top value
 * @param left the left value
 * @param tiles the tile count
 * @param lines the line buffer
 */
static void _draw_outline(uint8_t *page, uint32_t top, uint32_t left,
                          uint32_t tiles, uint32_t lines)
{
    uint32_t x0 = left, x1 = left + (tiles + 1u) * 16u;
    uint32_t y0 = top + 16u, y1 = y0 + lines * 16u;

    if (x1 > 320u) x1 = 320u;
    if (y1 > 200u) y1 = 200u;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = x0; x < x1; x++) {
            uint8_t v = page[y * 320u + x];
            if (v < 0xF4u || v >= 0xFCu) continue;
            if (y && page[(y - 1u) * 320u + x] < 0xF4u)
                page[(y - 1u) * 320u + x] = 0;
            if (y + 1u < 200u && page[(y + 1u) * 320u + x] < 0xF4u)
                page[(y + 1u) * 320u + x] = 0;
            if (x && page[y * 320u + x - 1u] < 0xF4u)
                page[y * 320u + x - 1u] = 0;
            if (x + 1u < 320u && page[y * 320u + x + 1u] < 0xF4u)
                page[y * 320u + x + 1u] = 0;
        }
}

void textpage_draw(uint8_t *page, const char *text, int32_t cursorRow,
                   uint32_t cursorCol, int32_t grid)
{
    if (!page || !text || !*text) return;

    uint32_t tiles, lines;
    _scroll_text_measure(text, &tiles, &lines);
    if (lines > 20u) return;

    uint32_t cols = tiles + 2u;
    uint32_t rows = lines + 2u;
    uint32_t left = 160u - cols * 8u;
    uint32_t top = 76u - rows * 8u;

    /* the 9-slice box: 0 TL, 1 L, 2 BL, 3 T, 4 C, 5 B, 6 TR, 7 R, 8 BR */
    if (_tp_box) {
        static const uint32_t tile[3][3] = {
            { 0x000u, 0x300u, 0x600u },
            { 0x100u, 0x400u, 0x700u },
            { 0x200u, 0x500u, 0x800u },
        };
        for (uint32_t ty = 0; ty < rows; ty++)
            for (uint32_t tx = 0; tx < cols; tx++) {
                uint32_t ry = (ty == 0u) ? 0u : (ty == rows - 1u) ? 2u : 1u;
                uint32_t rx = (tx == 0u) ? 0u : (tx == cols - 1u) ? 2u : 1u;
                _blit_box_tile(page, left + tx * 16u, top + ty * 16u,
                               _tp_box + tile[ry][rx]);
            }
    }

    /* the lines (blit_text_tile's 16-row pitch, draw_text's centring) */
    {
        const char *p = text;
        char buf[TEXT_LIMIT];
        for (uint32_t i = 0; i < lines; i++) {
            uint32_t n = 0;
            while (*p && *p != '\r' && n + 1 < sizeof buf) buf[n++] = *p++;
            buf[n] = 0;
            if (*p == '\r') p++;
            _draw_line(page, buf, 0xA0u, top + 15u + i * 16u, 0x10u, 1);
        }
    }

    /* the black outline (draw_map_object 1000:9FA7 -> 9FB1), after the lines
     * and before the markers — dialog_menu_loop draws those last */
    _draw_outline(page, top, left, tiles, lines);

    /* the cursor: the pause rows' edge markers, or the grid's cell pair */
    if (cursorRow >= 0) {
        uint32_t y = top + 51u + (uint32_t)cursorRow * 16u;
        if (grid) {
            /* the selected cell: the line's centring + the prefix width */
            const char *p = text;
            for (uint32_t i = 0; i < 2u + (uint32_t)cursorRow && *p; i++) {
                while (*p && *p != '\r') p++;
                if (*p == '\r') p++;
            }
            char line[TEXT_LIMIT];
            uint32_t n = 0;
            while (p[n] && p[n] != '\r' && n + 1 < sizeof line) {
                line[n] = p[n];
                n++;
            }
            line[n] = 0;
            uint32_t pre = 1u + 3u * cursorCol;      /* " 01 05 ..." */
            char prebuf[24];
            uint32_t k = pre < n ? pre : n;
            memcpy(prebuf, line, k);
            prebuf[k] = 0;
            int32_t cx = 160 - (int32_t)(font_text_width(line) / 2u) +
                         (int32_t)font_text_width(prebuf) - 9;
            if (cx < 0) cx = 0;
            _blit_marker(page, (uint32_t)cx, y, _marker_l);
            _blit_marker(page, (uint32_t)cx + 8u + font_text_width("00") + 2u,
                         y, _marker_r);
        } else {
            _blit_marker(page, left + 16u, y, _marker_l);
            _blit_marker(page, left + 16u * tiles + 8u, y, _marker_r);
        }
    }
}
