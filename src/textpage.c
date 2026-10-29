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
 *   A203 + A266                   the option markers: the 8x11 patterns at
 *                                 202F:0x5D33 (left) / 0x5D8B (right) /
 *                                 0x5DE3 (blank), placed 16 px inside the
 *                                 box's left edge and 8 px inside its right
 *                                 edge, at boxTop+51+16*row; the grid puts the
 *                                 same pair around the selected cell.
 *
 * The palette: the tile bytes are indices into the level's palette (0..63)
 * and the text/marker indices 0xF4 / 0xE3/0xE4 are the bar's bank (0xC0..0xFF,
 * level_video_init's panel4 copy) — exactly what the live capture shows.
 * The level DAC base is the working palette 202F:0x14D3 (level.c; re_map.md
 * "The level's working palette") and the box's indices ride it.  The port's
 * font is 1-bpp, so the lives' three-level glyph shading collapses to the
 * text colour 0xF4 (a cosmetic deviation).
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

static void draw_line(uint8_t *page, const char *line, unsigned cell,
                      unsigned colour)
{
    memset(tp_arena, 0, sizeof tp_arena);
    font_drawText(tp_arena, line, 0xA0u, 0);
    for (unsigned r = 4u; r < 14u; r++) {
        unsigned y = cell + r;
        if (y >= 200u) break;
        for (unsigned c = 0; c < 80u; c++) {
            uint8_t b = tp_arena[r * 80u + c];
            if (!b) continue;
            for (unsigned p = 0; p < 4u; p++)
                if (b & (1u << p)) {
                    unsigned x = c * 4u + p;
                    if (x < 320u) page[y * 320u + x] = (uint8_t)colour;
                }
        }
    }
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
            draw_line(page, buf, top + 15u + i * 16u, 0xF4u);
        }
    }

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
