/* The in-level dialogs: the pause menu and the SAVE GAME number grid.
 *
 *   pause_toggle   1000:3523      ACTION bit, no direction held
 *   the A167 loop  1000:A167      input, wrap, ACTION/Enter select
 *   the save grid  1000:A312..A3AD (202F:0x5CD8, game 4c+r+1)
 *
 * The countdown is suspended and the world keeps animating behind the dialog.
 */

#ifndef HUMANSRE_MENU_H
#define HUMANSRE_MENU_H

#include "osystem.h"

/**
 * @brief Bind the menu module to the backend.
 *
 * @param osys the backend interface
 */
void menu_init(const OSystem *osys);

/**
 * @brief Open the pause menu ([0x5B12] := 0, cursor 0).
 */
void menu_open_pause(void);

/**
 * @brief input_dispatch_loop 34F9: open the 3-option time-up block when
 * [0x5E44] != 0.
 *
 * A167's loop; its result is dispatched inline, 0-based: 0 restart this level
 * (level_restore), 1 restart this game (the room), 2 quit.
 */
void menu_open_time_up(void);

/**
 * @brief Accessor: the dialog currently open is the time-up variant.
 *
 * @return 1 for the time-up block, 0 for the pause menu
 */
uint8_t menu_time_up(void);

/**
 * @brief Accessor: the pause/time-up menu is up.
 *
 * @return 1 while a dialog is open, 0 otherwise
 */
uint8_t menu_active(void);

/**
 * @brief One menu tick; 1 = still up; draws.
 *
 * @param in the input state
 * @return 1 while the menu is up
 */
uint8_t menu_tick(const InputState *in);

/**
 * @brief Draw the menu.
 */
void menu_draw(void);

/**
 * @brief Accessor: the selected entry, the 0-based pause choice.
 *
 * The original's result is A167's AX, read from `[0x5E3B]` (1000:A1FD), so an
 * `int16_t`; -1 while nothing is chosen.
 *
 * @return the selected entry, or -1
 */
int16_t menu_result(void);

/**
 * @brief Accessor: the grid's game number 1..20.
 *
 * The original computes it in AX (`save_progress`, 1FE7:0008:
 * `[0x5E3D]*4 + [0x5E3B]`) and turns it into the drive letter.
 *
 * @return the selected slot
 */
uint8_t menu_slot(void);

/**
 * @brief Close the menu.
 */
void menu_close(void);

#endif /* HUMANSRE_MENU_H */
