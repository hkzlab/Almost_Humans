#include "level.h"
#include "menu.h"
#include "game_strs.h"
#include "osystem.h"
#include "statusbar.h"
#include "textpage.h"

#include <stdint.h>
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

/* 202F:0x5BBC+1 and 202F:0x5CD8+1, 202F:0x5C2B+1 - the pause block, the
 * 4x5 column-major SAVE GAME grid (games 1..20) and the time-up block, all in
 * game_strs.  The blocks' leading count byte ([0x5E41], not part of the text)
 * is not stored. */

static const OSystem *_menu_osys;
static MenuPhase _phase = MENU_NONE;
static uint16_t _row, _col;   // [0x5E3B]/[0x5E3D]: the original's words
static int16_t _result = -1;  // the 0-based pause choice (A167's AX)
static uint8_t _slot;         // the grid's game number 1..20
static uint8_t _prev_bits;    // the [0x5ACE] press-edge latch
static uint8_t _time_up_kind; // the open dialog is the time-up one

void menu_init(const OSystem *osys) { _menu_osys = osys; }

uint8_t menu_active(void)
{
    return _phase == MENU_PAUSE || _phase == MENU_GRID ||
           _phase == MENU_TIMEOUT;
}
uint8_t menu_time_up(void) { return _time_up_kind; }
int16_t menu_result(void) { return _result; }
uint8_t menu_slot(void) { return _slot; }
void menu_close(void)
{
    _phase = MENU_NONE;
    _result = -1;
    _prev_bits = 0;
    _time_up_kind = 0;
    /* D-232: the play loop resumes update_world after the dialog returns. */
    level_set_updates(1);
}

void menu_open_pause(void)
{
    _phase = MENU_PAUSE;
    _row = 0;
    _result = -1;
    _prev_bits = 0;
    _time_up_kind = 0;
    statusbar_set_active(0); // [0x5B12] := 0
    /* D-232: 1000:A167 replaces the play loop and only draws; it never calls
     * update_world (1000:80AD), so the pterodactyl, the marker arrow and the
     * object animations freeze. */
    level_set_updates(0);
}

void menu_open_time_up(void)
{
    _phase = MENU_TIMEOUT;
    _row = 0;
    _result = -1;
    _prev_bits = 0;
    _time_up_kind = 1;
    statusbar_set_active(0);
    level_set_updates(0); // D-232: the same draw-only A167 loop
}

void menu_draw(void)
{
    uint8_t *page = _menu_osys ? _menu_osys->videoPage(0) : NULL;
    if (_phase == MENU_PAUSE)
        textpage_draw(page, game_strs[g_game_lang][GAME_STR_MENU_PAUSE_BLOCK],
                      (int32_t)_row, 0u, 0);
    else if (_phase == MENU_GRID)
        textpage_draw(page, game_strs[g_game_lang][GAME_STR_MENU_GRID_BLOCK],
                      (int32_t)_row, _col, 1);
    else if (_phase == MENU_TIMEOUT)
        textpage_draw(page, game_strs[g_game_lang][GAME_STR_MENU_TIME_UP_BLOCK],
                      (int32_t)_row, 0u, 0);
}

uint8_t menu_tick(const InputState *in)
{
    if (_phase == MENU_DONE || _phase == MENU_NONE) return 0;

    menu_draw();

    uint8_t bits = in->joyBits; // [0x5ACE], a byte
    uint8_t press = (uint8_t)(bits & ~_prev_bits); // one move per press
    _prev_bits = bits;

    int16_t key = in->lastKey ? (int16_t)in->lastKey : -1;

    if (_phase == MENU_PAUSE || _phase == MENU_TIMEOUT) {
        uint8_t nrows = (_phase == MENU_PAUSE) ? MENU_ROWS : 3u; // [0x5E41]
        if (press & 0x01u) _row = _row ? _row - 1u : nrows - 1u;
        else if (press & 0x02u) _row = (_row + 1u) % nrows;
        if ((press & 0x80u) || key == 0x0D) {
            if (_phase == MENU_TIMEOUT) {
                _result = (int16_t)_row;
                _phase = MENU_DONE;
            } else if (_row == 3u) { // SAVE GAME -> the grid
                _phase = MENU_GRID;
                _row = 0;
                _col = 0;
                /* the same tick has already drawn the pause over a page the
                 * level did not re-render since; the grid draws next tick */
                return 1;
            } else {
                _result = (int16_t)_row;
                _phase = MENU_DONE;
            }
        }
    } else { // MENU_GRID
        if (press & 0x01u) _row = _row ? _row - 1u : GRID_ROWS - 1u;
        else if (press & 0x02u) _row = (_row + 1u) % GRID_ROWS;
        else if (press & 0x08u) _col = (_col + 1u) % GRID_COLS;
        else if (press & 0x04u) _col = _col ? _col - 1u : GRID_COLS - 1u;
        if ((press & 0x80u) || key == 0x0D) {
            _slot = (uint8_t)(4u * _col + _row + 1u); // the 4c+r+1 law
            _result = 3;
            _phase = MENU_DONE;
        }
    }
    if (key == 0x0D) ((InputState *)in)->lastKey = 0u; // consume Enter
    return _phase != MENU_DONE;
}
