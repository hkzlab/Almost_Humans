/* menu.c — the in-level dialogs: the pause menu and the SAVE GAME number
 * grid (re_map.md §"The in-level pause menu", re_formats.md §10.0).
 *
 * BEHAVIOUR (ported from the listing):
 *  - pause_toggle (1000:3523) opens the menu on the ACTION bit (0x80) with
 *    no direction held; the frame web's `A167` loop (1000:A167) then owns
 *    the input: bit 0x01 = row-1 (wrap to the last), bit 0x02 = row+1 (wrap
 *    to 0), ACTION or Enter (0x0D) = select; one move per press (the
 *    original waits for the key release).  The countdown is suspended
 *    ([0x5B12] = 0) while a dialog is up; the world keeps animating behind
 *    it (A167 re-runs the frame calls each iteration).
 *  - the 5 options are the 0-based results of `dispatch_by_result_a167`
 *    (1000:38EA): 0 continue, 1 restart this level, 2 restart this game,
 *    3 save (the number grid below), 4 quit.
 *  - SAVE GAME's grid is the 4-row x 5-column column-major block at
 *    202F:0x5CD8 (games 1..20, cell row r / column c = game 4c+r+1); its
 *    cursor is `render_frame_loop`'s [0x5E3B] (row) / [0x5E3D] (column),
 *    with up/down wrapping inside the 4 rows and left/right wrapping across
 *    the 5 columns (1000:A312..A3AD).
 *
 * The dialog is drawn by the original renderer: `menu_draw` hands the block to
 * `textpage_draw`, which is `draw_map_object` (1000:9EA3, the 9-slice
 * `blit_sprite_planar` box from APL\BOX.SPR) plus `blit_text_tile`'s glyph
 * pass with its `GC3 = 0x1003` OR and mask 0x10 shading.  (An earlier revision
 * drew it with the TEXT.SPR font on the flat page and carried a DEVIATION note
 * here; that renderer landed in pass 10/12 and the note was stale.)  The
 * pause block is opened by `gameflow_keyList` (ESC, 1000:386E event 4).
 */
#include "menu.h"
#include "osystem.h"
#include "statusbar.h"
#include "textpage.h"
#include <string.h>

#define MENU_ROWS 5u
#define GRID_ROWS 4u
#define GRID_COLS 5u

typedef enum {
    MENU_NONE = 0,
    MENU_PAUSE,
    MENU_GRID,
    MENU_TIMEOUT,
    MENU_DONE
} MenuPhase;

/* 202F:0x5BBC+1 — "PLEASE SELECT YOUR OPTION", then the 5 options (the
 * block's leading count byte is [0x5E41], not part of the text). */
static const char pauseBlock[] = "PLEASE SELECT YOUR OPTION\r"
                                 "\r"
                                 "CONTINUE THIS LEVEL\r"
                                 "RESTART THIS LEVEL\r"
                                 "RESTART THIS GAME\r"
                                 "SAVE GAME \r"
                                 "QUIT THIS GAME";

/* 202F:0x5CD8+1 — the 4x5 column-major grid, games 1..20. */
static const char gridBlock[] = "SAVE GAME NUMBER\r"
                                "\r"
                                " 01 05 09 13 17 \r"
                                " 02 06 10 14 18 \r"
                                " 03 07 11 15 19 \r"
                                " 04 08 12 16 20 ";

/* 202F:0x5C2B+1 — the time-up block's 3 options (the same renderer). */
static const char timeUpBlock[] = "YOU RAN OUT OF TIME!!\r"
                                  " \r"
                                  "RESTART THIS LEVEL\r"
                                  "RESTART THIS GAME\r"
                                  "QUIT THIS GAME";

static const OSystem *menu_osys;
static MenuPhase phase = MENU_NONE;
static uint32_t row, col;        /* [0x5E3B], [0x5E3D]                  */
static int32_t result = -1;     /* the 0-based pause choice            */
static uint32_t slot;            /* the grid's game number 1..20        */
static uint32_t prevBits;        /* the press-edge latch                */
static int32_t timeUpKind;      /* the open dialog is the time-up one  */

void menu_init(const OSystem *osys) { menu_osys = osys; }

int32_t menu_active(void)
{ return phase == MENU_PAUSE || phase == MENU_GRID || phase == MENU_TIMEOUT; }
int32_t menu_timeUp(void) { return timeUpKind; }
int32_t menu_result(void) { return result; }
uint32_t menu_slot(void) { return slot; }
void menu_close(void)
{
    phase = MENU_NONE;
    result = -1;
    prevBits = 0;
    timeUpKind = 0;
}

void menu_openPause(void)
{
    phase = MENU_PAUSE;
    row = 0;
    result = -1;
    prevBits = 0;
    timeUpKind = 0;
    statusbar_setActive(0);         /* [0x5B12] := 0                     */
}

/* input_dispatch_loop 34F9: [0x5E44] != 0 shows the 3-option time-up block
 * (A167's loop; the result is dispatched inline, 0-based: 0 restart this
 * level -> level_restore, 1 restart this game -> the room, 2 quit). */
void menu_openTimeUp(void)
{
    phase = MENU_TIMEOUT;
    row = 0;
    result = -1;
    prevBits = 0;
    timeUpKind = 1;
    statusbar_setActive(0);
}

void menu_draw(void)
{
    uint8_t *page = menu_osys ? menu_osys->videoPage(0) : NULL;
    if (phase == MENU_PAUSE)
        textpage_draw(page, pauseBlock, (int32_t)row, 0u, 0);
    else if (phase == MENU_GRID)
        textpage_draw(page, gridBlock, (int32_t)row, col, 1);
    else if (phase == MENU_TIMEOUT)
        textpage_draw(page, timeUpBlock, (int32_t)row, 0u, 0);
}

int32_t menu_tick(const InputState *in)
{
    if (phase == MENU_DONE || phase == MENU_NONE) return 0;

    menu_draw();

    uint32_t bits = in->joyBits;
    uint32_t press = bits & ~prevBits;        /* one move per press */
    prevBits = bits;

    int32_t key = in->lastKey ? (int32_t)in->lastKey : -1;

    if (phase == MENU_PAUSE || phase == MENU_TIMEOUT) {
        uint32_t nrows = (phase == MENU_PAUSE) ? MENU_ROWS : 3u;
        if (press & 0x01u) row = row ? row - 1u : nrows - 1u;
        else if (press & 0x02u) row = (row + 1u) % nrows;
        if ((press & 0x80u) || key == 0x0D) {
            if (phase == MENU_TIMEOUT) {
                result = (int32_t)row;
                phase = MENU_DONE;
            } else if (row == 3u) {            /* SAVE GAME -> the grid */
                phase = MENU_GRID;
                row = 0;
                col = 0;
                /* the same tick has already drawn the pause over a page the
                 * level did not re-render since; the grid draws next tick */
                return 1;
            } else {
                result = (int32_t)row;
                phase = MENU_DONE;
            }
        }
    } else {                                   /* MENU_GRID */
        if (press & 0x01u) row = row ? row - 1u : GRID_ROWS - 1u;
        else if (press & 0x02u) row = (row + 1u) % GRID_ROWS;
        else if (press & 0x08u) col = (col + 1u) % GRID_COLS;
        else if (press & 0x04u) col = col ? col - 1u : GRID_COLS - 1u;
        if ((press & 0x80u) || key == 0x0D) {
            slot = 4u * col + row + 1u;        /* the 4c+r+1 law */
            result = 3;
            phase = MENU_DONE;
        }
    }
    if (key == 0x0D) ((InputState *)in)->lastKey = 0u;  /* consume Enter */
    return phase != MENU_DONE;
}
