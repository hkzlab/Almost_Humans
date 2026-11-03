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

void menu_init(const OSystem *osys);
void menu_openPause(void); /* [0x5B12] := 0, cursor 0         */
void menu_openTimeUp(void); /* [0x5E44]'s 3-option block        */
int32_t menu_timeUp(void); /* the dialog currently open        */
int32_t menu_active(void); /* 1 while a dialog is up          */
int32_t menu_tick(const InputState *in); /* 1 = still up; draws         */
void menu_draw(void);
int32_t menu_result(void); /* the 0-based pause choice, -1    */
uint32_t menu_slot(void); /* the grid's game number 1..20    */
void menu_close(void);

#endif /* HUMANSRE_MENU_H */
