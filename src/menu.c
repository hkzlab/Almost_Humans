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
 * DEVIATION (port-only, documented): the original draws the dialog through
 * `draw_map_object` (1000:9EA3) — a 9-slice box blitted with
 * `blit_sprite_planar` (1000:A10E) and the text through `blit_text_tile`
 * (1000:47D5) on the mode-X level page.  The port has neither the mode-X
 * level page nor the box tile set yet, so it draws the same block with the
 * TEXT.SPR font on the flat page (a filled box, the same lines, the same
 * box geometry from `draw_scroll_text`'s measure, the palette-bank colours
 * the original's patterns use: 0xE4 fill, 0xE3 marker, 0xF4 text,
 * 0xD7 border).  The
 * text renderer's exact pass is the remaining fidelity item.
 */
#include "game.h"
#include <string.h>

#define MENU_ROWS  5u
#define GRID_ROWS  4u
#define GRID_COLS  5u

/* 202F:0x5BBC+1 — "PLEASE SELECT YOUR OPTION", then the 5 options (the
 * block's leading count byte is [0x5E41], not part of the text). */
static const char pauseBlock[] =
    "PLEASE SELECT YOUR OPTION\r"
    "\r"
    "CONTINUE THIS LEVEL\r"
    "RESTART THIS LEVEL\r"
    "RESTART THIS GAME\r"
    "SAVE GAME \r"
    "QUIT THIS GAME";

/* 202F:0x5CD8+1 — the 4x5 column-major grid, games 1..20. */
static const char gridBlock[] =
    "SAVE GAME NUMBER\r"
    "\r"
    " 01 05 09 13 17 \r"
    " 02 06 10 14 18 \r"
    " 03 07 11 15 19 \r"
    " 04 08 12 16 20 ";

/* 202F:0x5C2B+1 — the time-up block's 3 options (the same renderer). */
static const char timeUpBlock[] =
    "YOU RAN OUT OF TIME!!\r"
    " \r"
    "RESTART THIS LEVEL\r"
    "RESTART THIS GAME\r"
    "QUIT THIS GAME";

typedef enum { MENU_NONE = 0, MENU_PAUSE, MENU_GRID, MENU_TIMEOUT,
               MENU_DONE } MenuPhase;

static const OSystem *menu_osys;
static MenuPhase phase = MENU_NONE;
static unsigned  row, col;        /* [0x5E3B], [0x5E3D]                  */
static int       result = -1;     /* the 0-based pause choice            */
static unsigned  slot;            /* the grid's game number 1..20        */
static unsigned  prevBits;        /* the press-edge latch                */
static int       timeUpKind;      /* the open dialog is the time-up one  */

void menu_init(const OSystem *osys) { menu_osys = osys; }

int  menu_active(void)
{
    return phase == MENU_PAUSE || phase == MENU_GRID || phase == MENU_TIMEOUT;
}
int  menu_timeUp(void) { return timeUpKind; }
int  menu_result(void)  { return result; }
unsigned menu_slot(void) { return slot; }
void menu_close(void)
{
    phase = MENU_NONE; result = -1; prevBits = 0; timeUpKind = 0;
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
        textpage_draw(page, pauseBlock, (int)row, 0u, 0);
    else if (phase == MENU_GRID)
        textpage_draw(page, gridBlock, (int)row, col, 1);
    else if (phase == MENU_TIMEOUT)
        textpage_draw(page, timeUpBlock, (int)row, 0u, 0);
}

int menu_tick(const InputState *in)
{
    if (phase == MENU_DONE || phase == MENU_NONE) return 0;

    menu_draw();

    unsigned bits = in->joyBits;
    unsigned press = bits & ~prevBits;        /* one move per press */
    prevBits = bits;

    int key = in->lastKey ? (int)in->lastKey : -1;

    if (phase == MENU_PAUSE || phase == MENU_TIMEOUT) {
        unsigned nrows = (phase == MENU_PAUSE) ? MENU_ROWS : 3u;
        if (press & 0x01u) row = row ? row - 1u : nrows - 1u;
        else if (press & 0x02u) row = (row + 1u) % nrows;
        if ((press & 0x80u) || key == 0x0D) {
            if (phase == MENU_TIMEOUT) {
                result = (int)row;
                phase = MENU_DONE;
            } else if (row == 3u) {            /* SAVE GAME -> the grid */
                phase = MENU_GRID;
                row = 0; col = 0;
                /* the same tick has already drawn the pause over a page the
                 * level did not re-render since; the grid draws next tick */
                return 1;
            } else {
                result = (int)row;
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
