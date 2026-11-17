/* The MAP module's shared declarations: the state cells more than one theme
 * reads or writes and the helpers that cross a theme boundary.  Nothing here
 * is part of the port's public API; include map.h for that.
 */
#ifndef HUMANSRE_MAP_INTERNAL_H
#define HUMANSRE_MAP_INTERNAL_H

#include "map/map.h"
#include "video.h"

#include <stdint.h>

#define MAP_PAGE VIDEO_SCREEN_BYTES
#define NAME_MAX 13u

/* 1000:0CBE's display list at [0x596]: {target x,y, current x,y}. */
struct MapGlyph {
    int16_t tx, ty;
    int16_t cx, cy;
};

/* --- state shared between the themes (defined in the owning theme) ------ */

/* map_wheel.c: the apl\alpha.spr pair (loaded by map_begin, read by the ring). */
extern uint8_t *_alpha_packed; // apl\alpha.spr, the 4bpp sheet
extern uint8_t *_alpha_sheet;  // its 4-plane expansion (64 KB)
/* map_wheel.c: [0x48] the ring angle and [0x596]'s per-glyph display list.
 * The flow turns the angle and writes the typed glyphs. */
extern uint16_t _map_angle;
extern char *_map_name;
extern struct MapGlyph _map_glyphs[NAME_MAX];

/* map_flow.c: the offscreen list page ([0x8A4]) the frame bands read. */
extern uint8_t *_map_off;

/* map_band.c: BAND.SPR and its staging page. */
extern uint8_t *_band_sheet; // apl\band.spr: 304 rows x 80 bytes
extern uint8_t _map_stage_ready;

/* --- helpers that cross a theme boundary -------------------------------- */

/** @brief Reset both cell lists (screen entry; nothing to erase yet).
 */
void _map_dirty_reset(void);

/** @brief 1000:12D3: de-interleave BAND.SPR's four plane bytes into the
 * staging page (plane p of byte address i is source[i*4+p]).
 */
void _map_stage_fill(void);

/** @brief 1000:09DA palette_animate: tick the scripts and blit a changed
 * channel (the flow runs it every menu-loop iteration).
 */
void _map_palette_animate(void);

/** @brief 1000:0A72 for all four channels: force the strip layer onto the page.
 *
 * `1000:0221`/`0x09DA` only blit a channel whose word changed, so a page
 * re-copy (1000:024B) would leave the display without its strips.  The
 * original's strips live on the display page and survive that offscreen copy;
 * the port's one-page model needs this full redraw.
 *
 * @param page the page buffer
 */
void _map_draw_strips(uint8_t *page);

#endif /* HUMANSRE_MAP_INTERNAL_H */
