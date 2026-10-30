/* map.c — menu_loop (1000:012A), the MAP screen after the ending.
 *
 * START (this pass): the screen's card and palette, the "WELL DONE!" line,
 * and the key/ACTION exit back to the attract.  The original's menu_loop then
 * draws the high-score table (hiscore.dat / the score cards) and the map
 * grid with the moving cursor; those tables are not ported yet — the card,
 * the font and the exit are the seam the rest will hang from.
 *
 * Entry state: entry 0x151A sets [0xF7E]=0 and calls draw_status_indicator
 * (the disc-0 probe) before menu_loop, so the asset loads from disc 0.
 */
#include "game.h"
#include <stdlib.h>
#include <string.h>

#define MAP_PAGE (320u * 200u)

static uint8_t *mapHigh;
static uint32_t mapHighSize;
static uint8_t *mapFont;
static int mapUp;
static int mapArmed;      /* the first tick ignores the key that skipped the
                           * ending cutscene */

int map_begin(void)
{
    if (mapUp) return 1;

    res_setDisc(0u);                            /* entry 0x151A [0xF7E]=0 */
    mapHigh = res_loadChunk("LBM\\HIGH.BIT", &mapHighSize, NULL);
    uint8_t *page = video_page(0);
    memset(page, 0, MAP_PAGE);
    if (mapHigh) {
        memcpy(page, mapHigh,
               mapHighSize < MAP_PAGE ? mapHighSize : MAP_PAGE);
        if (mapHighSize >= MAP_PAGE + 768u)
            palette_setDac(mapHigh + MAP_PAGE, 0, 256);
    }

    mapFont = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(mapFont);
    textpage_drawLine(page, "WELL DONE!", 0x10u, 0x0Fu);

    mapUp = 1;
    mapArmed = 0;
    return 1;
}

int map_tick(const InputState *in)
{
    if (!mapUp) return 0;
    if (!mapArmed) { mapArmed = 1; return 1; }
    if (in->lastKey || (in->joyBits & 0x80u)) {
        mapUp = 0;
        free(mapHigh); mapHigh = NULL;
        free(mapFont); mapFont = NULL;
        font_publish(NULL);
        return 0;
    }
    return 1;
}
