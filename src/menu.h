/* menu.h — the interface of src/menu.c: the in-level dialogs (pause menu + SAVE GAME)
 *
 * The 5-option pause menu (202F:0x5BBC, A167) and the SAVE GAME number grid
 * (202F:0x5CD8, render_frame_loop's cursor): the behaviour is the original's
 * (0-based rows, wrap, ACTION/Enter selects; the countdown is suspended).
 * The dialog draws through draw_map_object (1000:9EA3); known deviations are
 * in port_deviations.md.
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

/** @brief input_dispatch_loop 34F9: [0x5E44] != 0 shows the 3-option time-up block
 * (A167's loop; the _result is dispatched inline, 0-based: 0 restart this
 * level -> level_restore, 1 restart this game -> the room, 2 quit).
 */
void menu_open_time_up(void);

/**
 * @brief Accessor: the menu is the time-up variant (the dialog currently open).
 *
 * @return The menu is the time-up variant.
 */
int32_t menu_time_up(void);

/**
 * @brief Accessor: the pause/time-up menu is up.
 *
 * @return The pause/time-up menu is up.
 */
int32_t menu_active(void);

/**
 * @brief One menu tick; 1 = still up; draws.
 *
 * @param in the input state
 * @return 1 while the menu is up
 */
int32_t menu_tick(const InputState *in);

/**
 * @brief Draw the menu.
 */
void menu_draw(void);

/**
 * @brief Accessor: the selected entry (the 0-based pause choice, -1).
 *
 * @return The selected entry.
 */
int32_t menu_result(void);

/**
 * @brief Accessor: the selected slot (the grid's game number 1..20).
 *
 * @return The selected slot.
 */
uint32_t menu_slot(void);

/**
 * @brief Close the menu.
 */
void menu_close(void);

#endif /* HUMANSRE_MENU_H */
