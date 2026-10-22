/* palette.c — fades and DAC (game_structure.md §1 subsystem 4, §6).
 * 6-bit palettes; palette_fade_step's state machine (DS:0x4EBC -> DAC). */
#include "game.h"

int palette_init(const OSystem *osys) { (void)osys; return 0; }
void palette_tick(void) {}
