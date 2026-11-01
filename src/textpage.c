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
#include "game.h"
#include <string.h>

#define TEXT_LIMIT 96u

static const uint8_t *tp_box;          /* APL\BOX.SPR: 9 x 16x16 tiles */
static uint8_t tp_arena[16000];

/* 202F:0x5D33 / 0x5D8B / 0x5DE3 — the 8x11 marker patterns (88 bytes). */
static const uint8_t markerL[88] = {
    0xE4,0xE3,0xE4,0xE4,0xE4,0xE4,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE4,0xE4,0xE4,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE4,0xE4,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE3,0xE4,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE3,0xE3,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE3,0xE3,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE3,0xE4,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE4,0xE4,0xE4,0xE4,
    0xE4,0xE3,0xE3,0xE4,0xE4,0xE4,0xE4,0xE4,
    0xE4,0xE3,0xE4,0xE4,0xE4,0xE4,0xE4,0xE4,
};
static const uint8_t markerR[88] = {
    0xE4,0xE4,0xE4,0xE4,0xE4,0xE4,0xE3,0xE4,
    0xE4,0xE4,0xE4,0xE4,0xE4,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE4,0xE4,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE4,0xE3,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE3,0xE3,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE3,0xE3,0xE3,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE3,0xE3,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE4,0xE3,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE4,0xE4,0xE3,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE4,0xE4,0xE4,0xE3,0xE3,0xE4,
    0xE4,0xE4,0xE4,0xE4,0xE4,0xE4,0xE3,0xE4,
};

void textpage_publishBox(const uint8_t *box) { tp_box = box; }

/* One line's measure: the advance sum (draw_text's own walk). */
static unsigned line_advance(const char *p, const char **end)
{
    char buf[64];
    unsigned n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\0' && n + 1 < sizeof buf) {
        buf[n] = p[n];
        n++;
    }
    buf[n] = 0;
    *end = p + n;
    return font_textWidth(buf);
}

/* draw_scroll_text (1000:A08E): the max line advance -> [0x5BB2] (tiles),
 * the line count -> [0x5BB4].  Lines are CR-separated, NUL-terminated. */
static void scroll_text_measure(const char *text, unsigned *tiles,
                                unsigned *lines)
{
    unsigned maxAdv = 1, nlines = 0;
    const char *p = text;
    for (;;) {
        const char *end;
        unsigned w = line_advance(p, &end);
        if (w > maxAdv) maxAdv = w;
        nlines++;
        if (*end == 0) break;
        p = end + 1;                    /* past the CR */
        if (*p == 0) { nlines++; break; }
    }
    *tiles = maxAdv / 16u + 2u;
    *lines = nlines;
}

/* blit_text_tile (1000:47D5) writes through write mode 2 with the OR ALU
 * (GC3 := 0x10 at 1000:9F76) and the bit mask = DL, so a glyph pixel is
 * `destination | mask`, never a solid colour: the shading comes from
 * whatever the box tile already put there.  `mask` != 0 selects that path;
 * `mask` == 0 writes the solid `colour` for the pages drawn outside a box. */
static void draw_line(uint8_t *page, const char *line, unsigned centreX,
                      unsigned cell, unsigned colour, int mask)
{
    memset(tp_arena, 0, sizeof tp_arena);
    font_drawText(tp_arena, line, centreX, 0);
    for (unsigned r = 4u; r < 14u; r++) {
        unsigned y = cell + r;
        if (y >= 200u) break;
        for (unsigned c = 0; c < 80u; c++) {
            uint8_t b = tp_arena[r * 80u + c];
            if (!b) continue;
            for (unsigned p = 0; p < 4u; p++)
                if (b & (1u << p)) {
                    unsigned x = c * 4u + p;
                    if (x < 320u) {
                        unsigned at = y * 320u + x;
                        if (mask) page[at] |= (uint8_t)colour;
                        else      page[at] = (uint8_t)colour;
                    }
                }
        }
    }
}

/* One centred line on the page at `row` in a solid `colour` (not the
 * blit_text_tile path — used where there is no box underneath). */
void textpage_drawLine(uint8_t *page, const char *line, unsigned row,
                       uint8_t colour)
{
    draw_line(page, line, 0xA0u, row, colour, 0);
}

/* The same line centred on an arbitrary x (draw_text's AX). */
void textpage_drawLineAt(uint8_t *page, const char *line, unsigned centreX,
                         unsigned row, uint8_t colour)
{
    draw_line(page, line, centreX, row, colour, 0);
}

/* One centred line blitted the original way: `mask` OR'd into the page.
 * The box's fill tile is a vertical ramp EB..E4..EB (all the same red in the
 * palette) so the text becomes the grey ramp FB..F4..FB; that is why the
 * level's goal text is shaded and not flat white. */
void textpage_drawLineMask(uint8_t *page, const char *line, unsigned row,
                           uint8_t mask)
{
    draw_line(page, line, 0xA0u, row, mask, 1);
}

/* The same masked line centred on an arbitrary x. */
void textpage_drawLineMaskAt(uint8_t *page, const char *line, unsigned centreX,
                             unsigned row, uint8_t mask)
{
    draw_line(page, line, centreX, row, mask, 1);
}

/* One 16x16 box tile (blit_sprite_planar A10E's layout). */
static void blit_box_tile(uint8_t *page, unsigned x0, unsigned y0,
                          const uint8_t *tile)
{
    for (unsigned r = 0; r < 16u; r++) {
        unsigned y = y0 + r;
        if (y >= 200u) continue;
        for (unsigned k = 0; k < 4u; k++)
            for (unsigned p = 0; p < 4u; p++) {
                unsigned x = x0 + k * 4u + p;
                if (x < 320u)
                    page[y * 320u + x] = tile[r * 16u + p + 4u * k];
            }
    }
}

/* The marker pair (A203/A266): 8x11, colour bytes as shipped. */
static void blit_marker(uint8_t *page, unsigned x0, unsigned y0,
                        const uint8_t *pat)
{
    for (unsigned r = 0; r < 11u; r++) {
        unsigned y = y0 + r;
        if (y >= 200u) continue;
        for (unsigned k = 0; k < 2u; k++)
            for (unsigned p = 0; p < 4u; p++) {
                unsigned x = x0 + k * 4u + p;
                if (x < 320u)
                    page[y * 320u + x] = pat[r * 8u + p + 4u * k];
            }
    }
}

/* text_page_outline (1000:9FB1, called at the tail of draw_map_object 9EA3):
 * a glyph pixel is any byte in [0xF4,0xFC) — the bar bank's grey ramp; every
 * 4-neighbour that is not one of those (< 0xF4) is set to 0, which is the
 * black outline the original draws around the box text.  The original walks
 * the four planes with the map mask/read map (1000:A012 + A025) and re-tests
 * per plane; with one byte per pixel the same rule is a single raster pass.
 * The scanned rectangle is [0x5D2F] = the box's interior base (box top + 16),
 * lines*16 rows, from the box's left edge for (tiles+1)*16 px (1000:9FBB). */
static void draw_outline(uint8_t *page, unsigned top, unsigned left,
                         unsigned tiles, unsigned lines)
{
    unsigned x0 = left, x1 = left + (tiles + 1u) * 16u;
    unsigned y0 = top + 16u, y1 = y0 + lines * 16u;

    if (x1 > 320u) x1 = 320u;
    if (y1 > 200u) y1 = 200u;
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++) {
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

void textpage_draw(uint8_t *page, const char *text, int cursorRow,
                   unsigned cursorCol, int grid)
{
    if (!page || !text || !*text) return;

    unsigned tiles, lines;
    scroll_text_measure(text, &tiles, &lines);
    if (lines > 20u) return;

    unsigned cols = tiles + 2u;
    unsigned rows = lines + 2u;
    unsigned left = 160u - cols * 8u;
    unsigned top = 76u - rows * 8u;

    /* the 9-slice box: 0 TL, 1 L, 2 BL, 3 T, 4 C, 5 B, 6 TR, 7 R, 8 BR */
    if (tp_box) {
        static const unsigned tile[3][3] = {
            {0x000u, 0x300u, 0x600u},
            {0x100u, 0x400u, 0x700u},
            {0x200u, 0x500u, 0x800u},
        };
        for (unsigned ty = 0; ty < rows; ty++)
            for (unsigned tx = 0; tx < cols; tx++) {
                unsigned ry = (ty == 0u) ? 0u
                            : (ty == rows - 1u) ? 2u : 1u;
                unsigned rx = (tx == 0u) ? 0u
                            : (tx == cols - 1u) ? 2u : 1u;
                blit_box_tile(page, left + tx * 16u, top + ty * 16u,
                              tp_box + tile[ry][rx]);
            }
    }

    /* the lines (blit_text_tile's 16-row pitch, draw_text's centring) */
    {
        const char *p = text;
        char buf[TEXT_LIMIT];
        for (unsigned i = 0; i < lines; i++) {
            unsigned n = 0;
            while (*p && *p != '\r' && n + 1 < sizeof buf) buf[n++] = *p++;
            buf[n] = 0;
            if (*p == '\r') p++;
            draw_line(page, buf, 0xA0u, top + 15u + i * 16u, 0x10u, 1);
        }
    }

    /* the black outline (draw_map_object 1000:9FA7 -> 9FB1), after the lines
     * and before the markers — dialog_menu_loop draws those last */
    draw_outline(page, top, left, tiles, lines);

    /* the cursor: the pause rows' edge markers, or the grid's cell pair */
    if (cursorRow >= 0) {
        unsigned y = top + 51u + (unsigned)cursorRow * 16u;
        if (grid) {
            /* the selected cell: the line's centring + the prefix width */
            const char *p = text;
            for (unsigned i = 0; i < 2u + (unsigned)cursorRow && *p; i++) {
                while (*p && *p != '\r') p++;
                if (*p == '\r') p++;
            }
            char line[TEXT_LIMIT];
            unsigned n = 0;
            while (p[n] && p[n] != '\r' && n + 1 < sizeof line) {
                line[n] = p[n];
                n++;
            }
            line[n] = 0;
            unsigned pre = 1u + 3u * cursorCol;      /* " 01 05 ..." */
            char prebuf[24];
            unsigned k = pre < n ? pre : n;
            memcpy(prebuf, line, k);
            prebuf[k] = 0;
            int cx = 160 - (int)(font_textWidth(line) / 2u)
                   + (int)font_textWidth(prebuf) - 9;
            if (cx < 0) cx = 0;
            blit_marker(page, (unsigned)cx, y, markerL);
            blit_marker(page, (unsigned)cx + 8u + font_textWidth("00") + 2u,
                        y, markerR);
        } else {
            blit_marker(page, left + 16u, y, markerL);
            blit_marker(page, left + 16u * tiles + 8u, y, markerR);
        }
    }
}
